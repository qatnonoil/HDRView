//
// Copyright (C) Wojciech Jarosz <wjarosz@gmail.com>. All rights reserved.
// Use of this source code is governed by a BSD-style license that can
// be found in the LICENSE.txt file.
//

#include "hdrimage.h"
#include <ctype.h>               // for tolower
#include <stdlib.h>              // for abs
#include <algorithm>             // for nth_element, transform
#include <cmath>                 // for floor, pow, exp, ceil, round, sqrt
#include <exception>             // for exception
#include <functional>            // for pointer_to_unary_function, function
#include <stdexcept>             // for runtime_error, out_of_range
#include <string>                // for allocator, operator==, basic_string
#include <vector>                // for vector
#include "common.h"              // for lerp, mod, clamp, getExtension
#include "colorspace.h"
#include "parallelfor.h"
#include "timer.h"
#include <spdlog/spdlog.h>


#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize.h"    // for stbir_resize_float


using namespace std;
using namespace Eigen;

// local functions
namespace
{

const Color4 g_blackPixel(0,0,0,0);

// create a vector containing the normalized values of a 1D Gaussian filter
ArrayXXf horizontalGaussianKernel(float sigma, float truncate);
int wrapCoord(int p, int maxP, HDRImage::BorderMode m);
void bilinearGreen(HDRImage &raw, int offsetX, int offsetY);
void PhelippeauGreen(HDRImage &raw, const Vector2i & redOffset);
void MalvarGreen(HDRImage &raw, int c, const Vector2i & redOffset);
void MalvarRedOrBlueAtGreen(HDRImage &raw, int c, const Vector2i &redOffset, bool horizontal);
void MalvarRedOrBlue(HDRImage &raw, int c1, int c2, const Vector2i &redOffset);
void bilinearRedBlue(HDRImage &raw, int c, const Vector2i & redOffset);
void greenBasedRorB(HDRImage &raw, int c, const Vector2i &redOffset);
inline float clamp2(float value, float mn, float mx);
inline float clamp4(float value, float a, float b, float c, float d);
inline float interpGreenH(const HDRImage &raw, int x, int y);
inline float interpGreenV(const HDRImage &raw, int x, int y);
inline float ghG(const ArrayXXf & G, int i, int j);
inline float gvG(const ArrayXXf & G, int i, int j);
inline int bayerColor(int x, int y);
inline Vector3f cameraToLab(const Vector3f c, const Matrix3f & cameraToXYZ, const vector<float> & LUT);
} // namespace


const vector<string> & HDRImage::borderModeNames()
{
	static const vector<string> names =
		{
			"Black",
			"Edge",
			"Repeat",
			"Mirror"
		};
	return names;
}

const vector<string> & HDRImage::samplerNames()
{
	static const vector<string> names =
		{
			"Nearest neighbor",
			"Bilinear",
			"Bicubic"
		};
	return names;
}

const Color4 & HDRImage::pixel(int x, int y, BorderMode mX, BorderMode mY) const
{
	x = wrapCoord(x, width(), mX);
	y = wrapCoord(y, height(), mY);
	if (x < 0 || y < 0)
		return g_blackPixel;

	return (*this)(x, y);
}

Color4 & HDRImage::pixel(int x, int y, BorderMode mX, BorderMode mY)
{
	x = wrapCoord(x, width(), mX);
	y = wrapCoord(y, height(), mY);
	if (x < 0 || y < 0)
		throw out_of_range("Cannot assign to out-of-bounds pixel when BorderMode==BLACK.");

	return (*this)(x, y);
}

Color4 HDRImage::sample(float sx, float sy, Sampler s, BorderMode mX, BorderMode mY) const
{
	switch (s)
	{
		case NEAREST:  return nearest(sx, sy, mX, mY);
		case BILINEAR: return bilinear(sx, sy, mX, mY);
		case BICUBIC:  return bicubic(sx, sy, mX, mY);
	}
}

Color4 HDRImage::nearest(float sx, float sy, BorderMode mX, BorderMode mY) const
{
    return pixel(std::floor(sx), std::floor(sy), mX, mY);
}

Color4 HDRImage::bilinear(float sx, float sy, BorderMode mX, BorderMode mY) const
{
    // shift so that pixels are defined at their centers
    sx -= 0.5f;
    sy -= 0.5f;

    int x0 = (int) std::floor(sx);
    int y0 = (int) std::floor(sy);
    int x1 = x0 + 1;
    int y1 = y0 + 1;
    sx -= x0;
    sy -= y0;

    return lerp(lerp(pixel(x0, y0, mX, mY), pixel(x1, y0, mX, mY), sx),
                lerp(pixel(x0, y1, mX, mY), pixel(x1, y1, mX, mY), sx), sy);
}

// photoshop bicubic
Color4 HDRImage::bicubic(float sx, float sy, BorderMode mX, BorderMode mY) const
{
    // shift so that pixels are defined at their centers
    sx -= 0.5f;
    sy -= 0.5f;

    int bx = (int) std::floor(sx);
    int by = (int) std::floor(sy);

    float A = -0.75f;
    float totalweight = 0;
    Color4 val(0, 0, 0, 0);

    for (int y = by - 1; y < by + 3; y++)
    {
        float disty = fabs(sy - y);
        float yweight = (disty <= 1) ?
            ((A + 2.0f) * disty - (A + 3.0f)) * disty * disty + 1.0f :
            ((A * disty - 5.0f * A) * disty + 8.0f * A) * disty - 4.0f * A;

        for (int x = bx - 1; x < bx + 3; x++)
        {
            float distx = fabs(sx - x);
            float weight = (distx <= 1) ?
                (((A + 2.0f) * distx - (A + 3.0f)) * distx * distx + 1.0f) * yweight :
                (((A * distx - 5.0f * A) * distx + 8.0f * A) * distx - 4.0f * A) * yweight;

            val += pixel(x, y, mX, mY) * weight;
            totalweight += weight;
        }
    }
    val *= 1.0f / totalweight;
    return val;
}


HDRImage HDRImage::resampled(int w, int h,
                             function<Vector2f(const Vector2f &)> warpFn,
                             int superSample, Sampler sampler, BorderMode mX, BorderMode mY) const
{
    HDRImage result(w, h);

    Timer timer;
    // for every pixel in the image
    parallel_for(0, result.height(), [this,w,h,&warpFn,&result,superSample,sampler,mX,mY](int y)
    {
        for (int x = 0; x < result.width(); ++x)
        {
            Color4 sum(0, 0, 0, 0);
            for (int yy = 0; yy < superSample; ++yy)
            {
                float j = (yy + 0.5f) / superSample;
                for (int xx = 0; xx < superSample; ++xx)
                {
                    float i = (xx + 0.5f) / superSample;
                    Vector2f srcUV = warpFn(Vector2f((x + i) / w, (y + j) / h)).array() * Array2f(width(), height());
                    sum += sample(srcUV(0), srcUV(1), sampler, mX, mY);
                }
            }
            result(x, y) = sum / (superSample * superSample);
        }
    });
    spdlog::get("console")->debug("Resampling took: {} seconds.", (timer.elapsed()/1000.f));
    return result;
}


HDRImage HDRImage::convolved(const ArrayXXf &kernel, BorderMode mX, BorderMode mY) const
{
    HDRImage imFilter(width(), height());

    int centerX = int((kernel.rows()-1.0)/2.0);
    int centerY = int((kernel.cols()-1.0)/2.0);

    Timer timer;
    // for every pixel in the image
    parallel_for(0, imFilter.width(), [this,kernel,mX,mY,&imFilter,centerX,centerY](int x)
    {
        for (int y = 0; y < imFilter.height(); y++)
        {
            Color4 accum(0.0f, 0.0f, 0.0f, 0.0f);
            float weightSum = 0.0f;
            // for every pixel in the kernel
            for (int xFilter = 0; xFilter < kernel.rows(); xFilter++)
            {
                int xx = x-xFilter+centerX;

                for (int yFilter = 0; yFilter < kernel.cols(); yFilter++)
                {
                    int yy = y-yFilter+centerY;
                    accum += kernel(xFilter, yFilter) * pixel(xx, yy, mX, mY);
                    weightSum += kernel(xFilter, yFilter);
                }
            }

            // assign the pixel the value from convolution
            imFilter(x,y) = accum / weightSum;
        }
    });
    spdlog::get("console")->debug("Convolution took: {} seconds.", (timer.elapsed()/1000.f));

    return imFilter;
}

HDRImage HDRImage::GaussianBlurredX(float sigmaX, BorderMode mX, float truncateX) const
{
    return convolved(horizontalGaussianKernel(sigmaX, truncateX), mX, mX);
}

HDRImage HDRImage::GaussianBlurredY(float sigmaY, BorderMode mY, float truncateY) const
{
    return convolved(horizontalGaussianKernel(sigmaY, truncateY).transpose(), mY, mY);
}

// Use principles of separability to blur an image using 2 1D Gaussian Filters
HDRImage HDRImage::GaussianBlurred(float sigmaX, float sigmaY, BorderMode mX, BorderMode mY,
                                   float truncateX, float truncateY) const
{
    // blur using 2, 1D filters in the x and y directions
    return GaussianBlurredX(sigmaX, mX, truncateX).GaussianBlurredY(sigmaY, mY, truncateY);
}


// sharpen an image
HDRImage HDRImage::unsharpMasked(float sigma, float strength, BorderMode mX, BorderMode mY) const
{
    return *this + Color4(strength) * (*this - fastGaussianBlurred(sigma, sigma, mX, mY));
}



HDRImage HDRImage::medianFiltered(float radius, int channel, BorderMode mX, BorderMode mY, bool round) const
{
    int radiusi = int(std::ceil(radius));
    HDRImage tempBuffer = *this;

    Timer timer;
    // for every pixel in the image
    parallel_for(0, height(), [this,&tempBuffer,radius,radiusi,channel,mX,mY,round](int y)
    {
        vector<float> mBuffer;
        mBuffer.reserve((2*radiusi)*(2*radiusi));
        for (int x = 0; x < tempBuffer.width(); x++)
        {
            mBuffer.clear();

            int xCoord, yCoord;
            // over all pixels in the neighborhood kernel
            for (int i = -radiusi; i <= radiusi; i++)
            {
                xCoord = x + i;
                for (int j = -radiusi; j <= radiusi; j++)
                {
                    if (round && i*i + j*j > radius*radius)
                        continue;

                    yCoord = y + j;
                    mBuffer.push_back(pixel(xCoord, yCoord, mX, mY)[channel]);
                }
            }

            int num = mBuffer.size();
            int med = (num-1)/2;

            nth_element(mBuffer.begin() + 0,
                        mBuffer.begin() + med,
                        mBuffer.begin() + mBuffer.size());
            tempBuffer(x,y)[channel] = mBuffer[med];
        }
    });
    spdlog::get("console")->debug("Median filter took: {} seconds.", (timer.elapsed()/1000.f));

    return tempBuffer;
}


HDRImage HDRImage::bilateralFiltered(float sigmaRange, float sigmaDomain, BorderMode mX, BorderMode mY, float truncateDomain) const
{
    HDRImage imFilter(width(), height());

    // calculate the filter size
    int radius = int(std::ceil(truncateDomain * sigmaDomain));

    Timer timer;
    // for every pixel in the image
    parallel_for(0, imFilter.width(), [this,&imFilter,radius,sigmaRange,sigmaDomain,mX,mY,truncateDomain](int x)
    {
        for (int y = 0; y < imFilter.height(); y++)
        {
            // initilize normalizer and sum value to 0 for every pixel location
            float weightSum = 0.0f;
            Color4 accum(0.0f, 0.0f, 0.0f, 0.0f);

            for (int xFilter = -radius; xFilter <= radius; xFilter++)
            {
                int xx = x+xFilter;
                for (int yFilter = -radius; yFilter <= radius; yFilter++)
                {
                    int yy = y+yFilter;

                    // calculate the squared distance between the 2 pixels (in range)
                    float rangeExp = ::pow(pixel(xx,yy,mX,mY) - (*this)(x,y), 2).sum();
                    float domainExp = std::pow(xFilter,2) + std::pow(yFilter,2);

                    // calculate the exponentiated weighting factor from the domain and range
                    float factorDomain = std::exp(-domainExp / (2.0 * std::pow(sigmaDomain,2)));
                    float factorRange = std::exp(-rangeExp / (2.0 * std::pow(sigmaRange,2)));
                    weightSum += factorDomain * factorRange;
                    accum += factorDomain * factorRange * pixel(xx,yy,mX,mY);
                }
            }

            // set pixel in filtered image to weighted sum of values in the filter region
            imFilter(x,y) = accum/weightSum;
        }
    });
    spdlog::get("console")->debug("Bilateral filter took: {} seconds.", (timer.elapsed()/1000.f));

    return imFilter;
}


static int nextOddInt(int i)
{
  return (i % 2 == 0) ? i+1 : i;
}


HDRImage HDRImage::iteratedBoxBlurred(float sigma, int iterations, BorderMode mX, BorderMode mY) const
{
    // Compute box blur size for desired sigma and number of iterations:
    // The kernel resulting from repeated box blurs of the same width is the
    // Irwin–Hall distribution
    // (https://en.wikipedia.org/wiki/Irwin–Hall_distribution)
    //
    // The variance of the Irwin-Hall distribution with n unit-sized boxes:
    //
    //      V(1, n) = n/12.
    //
    // Since V[w * X] = w^2 V[X] where w is a constant, we know that the
    // variance will scale as follows using width-w boxes:
    //
    //      V(w, n) = w^2*n/12.
    //
    // To achieve a certain standard deviation sigma, we want to find solve:
    //
    //      sqrt(V(w, n)) = w*sqrt(n/12) = sigma
    //
    // for w, given n and sigma; which is:
    //
    //      w = sqrt(12/n)*sigma
    //

    int w = nextOddInt(std::round(std::sqrt(12.f/iterations) * sigma));

    // Now, if width is odd, then we can use a centered box and are good to go.
    // If width is even, then we can't use centered boxes, but must instead
    // use a symmetric pairs of off-centered boxes. For now, just always round
    // up to next odd width
    int hw = (w-1)/2;

    HDRImage imFilter = *this;
    for (int i = 0; i < iterations; i++)
        imFilter = imFilter.boxBlurred(hw, mX, mY);

    return imFilter;
}

HDRImage HDRImage::fastGaussianBlurred(float sigmaX, float sigmaY, BorderMode mX, BorderMode mY) const
{
    Timer timer;
    // See comments in HDRImage::iteratedBoxBlurred for derivation of width
    int hw = std::round((std::sqrt(12.f/6) * sigmaX - 1)/2.f);
    int hh = std::round((std::sqrt(12.f/6) * sigmaY - 1)/2.f);

    HDRImage im;
    // do horizontal blurs
    if (hw < 3)
        // for small blurs, just use a separable Gaussian
        im = GaussianBlurredX(sigmaX, mX);
    else
        // for large blurs, approximate Gaussian with 6 box blurs
        im = boxBlurredX(hw, mX).boxBlurredX(hw, mX).boxBlurredX(hw, mX).
            boxBlurredX(hw, mX).boxBlurredX(hw, mX).boxBlurredX(hw, mX);

    // now do vertical blurs
    if (hh < 3)
        // for small blurs, just use a separable Gaussian
        im = im.GaussianBlurredY(sigmaY, mY);
    else
        // for large blurs, approximate Gaussian with 6 box blurs
        im = im.boxBlurredY(hh, mY).boxBlurredY(hh, mY).boxBlurredY(hh, mY).
            boxBlurredY(hh, mY).boxBlurredY(hh, mY).boxBlurredY(hh, mY);

    spdlog::get("console")->debug("fastGaussianBlurred filter took: {} seconds.", (timer.elapsed()/1000.f));
    return im;
}


HDRImage HDRImage::boxBlurredX(int leftSize, int rightSize, BorderMode mX) const
{
    HDRImage imFilter(width(), height());

    Timer timer;
    // for every pixel in the image
    parallel_for(0, imFilter.height(), [this,&imFilter,leftSize,rightSize,mX](int y)
    {
        // fill up the accumulator
        imFilter(0, y) = 0;
        for (int dx = -leftSize; dx <= rightSize; ++dx)
            imFilter(0, y) += pixel(dx, y, mX, mX);

        for (int x = 1; x < width(); ++x)
            imFilter(x, y) = imFilter(x-1, y) -
                             pixel(x-1-leftSize, y, mX, mX) +
                             pixel(x+rightSize, y, mX, mX);
    });
    spdlog::get("console")->debug("boxBlurredX filter took: {} seconds.", (timer.elapsed()/1000.f));

    return imFilter * Color4(1.f/(leftSize + rightSize + 1));
}


HDRImage HDRImage::boxBlurredY(int leftSize, int rightSize, BorderMode mY) const
{
    HDRImage imFilter(width(), height());

    Timer timer;
    // for every pixel in the image
    parallel_for(0, imFilter.width(), [this,&imFilter,leftSize,rightSize,mY](int x)
    {
        // fill up the accumulator
        imFilter(x, 0) = 0;
        for (int dy = -leftSize; dy <= rightSize; ++dy)
            imFilter(x, 0) += pixel(x, dy, mY, mY);

        for (int y = 1; y < height(); ++y)
            imFilter(x, y) = imFilter(x, y-1) -
                             pixel(x, y-1-leftSize, mY, mY) +
                             pixel(x, y+rightSize, mY, mY);
    });
    spdlog::get("console")->debug("boxBlurredX filter took: {} seconds.", (timer.elapsed()/1000.f));

    return imFilter * Color4(1.f/(leftSize + rightSize + 1));
}

HDRImage HDRImage::resizedCanvas(int newW, int newH, CanvasAnchor anchor, const Color4 & bgColor) const
{
    int oldW = width();
    int oldH = height();

    // fill in new regions with border value
    HDRImage img = HDRImage::Constant(newW, newH, bgColor);

    Vector2i tlDst(0,0);
    // find top-left corner
    switch (anchor)
    {
        case HDRImage::TOP_RIGHT:
        case HDRImage::MIDDLE_RIGHT:
        case HDRImage::BOTTOM_RIGHT:
            tlDst.x() = newW-oldW;
            break;

        case HDRImage::TOP_CENTER:
        case HDRImage::MIDDLE_CENTER:
        case HDRImage::BOTTOM_CENTER:
            tlDst.x() = (newW-oldW)/2;
            break;

        case HDRImage::TOP_LEFT:
        case HDRImage::MIDDLE_LEFT:
        case HDRImage::BOTTOM_LEFT:
        default:
            tlDst.x() = 0;
            break;
    }
    switch (anchor)
    {
        case HDRImage::BOTTOM_LEFT:
        case HDRImage::BOTTOM_CENTER:
        case HDRImage::BOTTOM_RIGHT:
            tlDst.y() = newH-oldH;
            break;

        case HDRImage::MIDDLE_LEFT:
        case HDRImage::MIDDLE_CENTER:
        case HDRImage::MIDDLE_RIGHT:
            tlDst.y() = (newH-oldH)/2;
            break;

        case HDRImage::TOP_LEFT:
        case HDRImage::TOP_CENTER:
        case HDRImage::TOP_RIGHT:
        default:
            tlDst.y() = 0;
            break;
    }

    Vector2i tlSrc(0,0);
    if (tlDst.x() < 0)
    {
        tlSrc.x() = -tlDst.x();
        tlDst.x() = 0;
    }
    if (tlDst.y() < 0)
    {
        tlSrc.y() = -tlDst.y();
        tlDst.y() = 0;
    }

    Vector2i bs(std::min(oldW, newW), std::min(oldH, newH));

    img.block(tlDst.x(), tlDst.y(),
              bs.x(), bs.y()) = block(tlSrc.x(), tlSrc.y(),
                                                    bs.x(), bs.y());
    return img;
}


HDRImage HDRImage::resized(int w, int h) const
{
    HDRImage newImage(w, h);

    if (!stbir_resize_float((const float *)data(), width(), height(), 0,
                            (float *) newImage.data(), w, h, 0, 4))
        throw runtime_error("Failed to resize image.");

    return newImage;
}

/*!
 * \brief Multiplies a raw image by the Bayer mosaic pattern so that only a single
 * R, G, or B channel is non-zero for each pixel.
 *
 * We assume the canonical Bayer pattern looks like:
 *
 * \rst
 * +---+---+
 * | R | G |
 * +---+---+
 * | G | B |
 * +---+---+
 *
 * \endrst
 *
 * and the pattern is tiled across the entire image.
 *
 * @param redOffset The x,y offset to the first red pixel in the Bayer pattern
 */
void HDRImage::bayerMosaic(const Vector2i &redOffset)
{
    Color4 mosaic[2][2] = {{Color4(1.f, 0.f, 0.f, 1.f), Color4(0.f, 1.f, 0.f, 1.f)},
                           {Color4(0.f, 1.f, 0.f, 1.f), Color4(0.f, 0.f, 1.f, 1.f)}};
    for (int y = 0; y < height(); ++y)
    {
        int r = mod(y - redOffset.y(), 2);
        for (int x = 0; x < width(); ++x)
        {
            int c = mod(x - redOffset.x(), 2);
            (*this)(x,y) *= mosaic[r][c];
        }
    }
}


/*!
 * \brief Compute the missing green pixels using a simple bilinear interpolation.
 * from the 4 neighbors.
 *
 * @param redOffset The x,y offset to the first red pixel in the Bayer pattern.
 */
void HDRImage::demosaicGreenLinear(const Vector2i &redOffset)
{
    bilinearGreen(*this, redOffset.x(), redOffset.y());
}

/*!
 * \brief Compute the missing green pixels using vertical linear interpolation.
 *
 * @param raw       The source raw pixel data.
 * @param redOffset The x,y offset to the first red pixel in the Bayer pattern.
 */
void HDRImage::demosaicGreenHorizontal(const HDRImage &raw, const Vector2i &redOffset)
{
    parallel_for(redOffset.y(), height(), 2, [this,&raw,&redOffset](int y)
    {
        for (int x = 2+redOffset.x(); x < width()-2; x += 2)
        {
            // populate the green channel into the red and blue pixels
            (*this)(x  , y  ).g = interpGreenH(raw, x, y);
            (*this)(x+1, y+1).g = interpGreenH(raw, x + 1, y + 1);
        }
    });
}

/*!
 * \brief Compute the missing green pixels using horizontal linear interpolation.
 *
 * @param raw       The source raw pixel data.
 * @param redOffset The x,y offset to the first red pixel in the Bayer pattern.
 */
void HDRImage::demosaicGreenVertical(const HDRImage &raw, const Vector2i &redOffset)
{
    parallel_for(2+redOffset.y(), height()-2, 2, [this,&raw,&redOffset](int y)
    {
        for (int x = redOffset.x(); x < width(); x += 2)
        {
            (*this)(x  , y  ).g = interpGreenV(raw, x, y);
            (*this)(x+1, y+1).g = interpGreenV(raw, x + 1, y + 1);
        }
    });
}

/*!
 * \brief Interpolate the missing green pixels using the method by Malvar et al. 2004.
 *
 * The method uses a plus "+" shaped 5x5 filter, which is linear, except--to reduce
 * ringing/over-shooting--the interpolation is not allowed to extrapolate higher or
 * lower than the surrounding green pixels.
 *
 * @param redOffset The x,y offset to the first red pixel in the Bayer pattern.
 */
void HDRImage::demosaicGreenMalvar(const Vector2i &redOffset)
{
    // fill in missing green at red pixels
    MalvarGreen(*this, 0, redOffset);
    // fill in missing green at blue pixels
    MalvarGreen(*this, 2, Vector2i((redOffset.x() + 1) % 2, (redOffset.y() + 1) % 2));
}

/*!
 * \brief Interpolate the missing green pixels using the method by Phelippeau et al. 2009.
 *
 * @param redOffset The x,y offset to the first red pixel in the Bayer pattern.
 */
void HDRImage::demosaicGreenPhelippeau(const Vector2i &redOffset)
{
    PhelippeauGreen(*this, redOffset);
}

/*!
 * \brief Interpolate the missing red and blue pixels using a simple linear or bilinear interpolation.
 *
 * @param redOffset The x,y offset to the first red pixel in the Bayer pattern.
 */
void HDRImage::demosaicRedBlueLinear(const Vector2i &redOffset)
{
    bilinearRedBlue(*this, 0, redOffset);
    bilinearRedBlue(*this, 2, Vector2i((redOffset.x() + 1) % 2, (redOffset.y() + 1) % 2));
}

/*!
 * \brief Interpolate the missing red and blue pixels using a linear or bilinear interpolation
 * guided by the green channel, which is assumed already demosaiced.
 *
 * The interpolation is equivalent to performing (bi)linear interpolation of the red-green and
 * blue-green differences, and then adding green back into the interpolated result. This inject
 * some of the higher resolution of the green channel, and reduces color fringing under the
 * assumption that the color channels in natural images are positively correlated.
 *
 * @param redOffset The x,y offset to the first red pixel in the Bayer pattern.
 */
void HDRImage::demosaicRedBlueGreenGuidedLinear(const Vector2i &redOffset)
{
    greenBasedRorB(*this, 0, redOffset);
    greenBasedRorB(*this, 2, Vector2i((redOffset.x() + 1) % 2, (redOffset.y() + 1) % 2));
}

/*!
 * \brief Interpolate the missing red and blue pixels using the method by Malvar et al. 2004.
 *
 * The interpolation for each channel is guided by the available information from all other
 * channels. The green channel is assumed to already be demosaiced.
 *
 * The method uses a 5x5 linear filter.
 *
 * @param redOffset The x,y offset to the first red pixel in the Bayer pattern.
 */
void HDRImage::demosaicRedBlueMalvar(const Vector2i &redOffset)
{
    // fill in missing red horizontally
    MalvarRedOrBlueAtGreen(*this, 0, Vector2i((redOffset.x() + 1) % 2, redOffset.y()), true);
    // fill in missing red vertically
    MalvarRedOrBlueAtGreen(*this, 0, Vector2i(redOffset.x(), (redOffset.y() + 1) % 2), false);

    // fill in missing blue horizontally
    MalvarRedOrBlueAtGreen(*this, 2, Vector2i(redOffset.x(), (redOffset.y() + 1) % 2), true);
    // fill in missing blue vertically
    MalvarRedOrBlueAtGreen(*this, 2, Vector2i((redOffset.x() + 1) % 2, redOffset.y()), false);

    // fill in missing red at blue
    MalvarRedOrBlue(*this, 0, 2, Vector2i((redOffset.x() + 1) % 2, (redOffset.y() + 1) % 2));
    // fill in missing blue at red
    MalvarRedOrBlue(*this, 2, 0, redOffset);
}

/*!
 * \brief Reduce some remaining color fringing and zipper artifacts by median-filtering the
 * red-green and blue-green differences as originally proposed by Freeman.
 */
HDRImage HDRImage::medianFilterBayerArtifacts() const
{
    HDRImage colorDiff = unaryExpr([](const Color4 & c){return Color4(c.r-c.g,c.g,c.b-c.g,c.a);});
    colorDiff = colorDiff.medianFiltered(1.f, 0).medianFiltered(1.f, 2);
    return binaryExpr(colorDiff, [](const Color4 & i, const Color4 & med){return Color4(med.r + i.g, i.g, med.b + i.g, i.a);}).eval();
}

/*!
 * \brief Demosaic the image using the "Adaptive Homogeneity-Directed" interpolation
 * approach proposed by Hirakawa et al. 2004.
 *
 * The approach is fairly expensive, but produces the best results.
 *
 * The method first creates two competing full-demosaiced images: one where the
 * green channel is interpolated vertically, and the other horizontally. In both
 * images the red and green are demosaiced using the corresponding green channel
 * as a guide.
 *
 * The two candidate images are converted to XYZ (using the supplied \a cameraToXYZ
 * matrix) subsequently to CIE L*a*b* space in order to determine how perceptually
 * "homogeneous" each pixel neighborhood is.
 *
 * "Homogeneity maps" are created for the two candidate imates which count, for each
 * pixel, the number of perceptually similar pixels among the 4 neighbors in the
 * cardinal directions.
 *
 * Finally, the output image is formed by choosing for each pixel the demosaiced
 * result which has the most homogeneous "votes" in the surrounding 3x3 neighborhood.
 *
 * @param redOffset     The x,y offset to the first red pixel in the Bayer pattern.
 * @param cameraToXYZ   The matrix that transforms from sensor values to XYZ with
 *                      D65 white point.
 */
void HDRImage::demosaicAHD(const Vector2i &redOffset, const Matrix3f &cameraToXYZ)
{
    typedef Array<Vector3f,Dynamic,Dynamic> Image3f;
    typedef Array<uint8_t,Dynamic,Dynamic> HomoMap;
    HDRImage rgbH = *this;
    HDRImage rgbV = *this;
    Image3f labH(width(), height());
    Image3f labV(width(), height());
    HomoMap homoH = HomoMap::Zero(width(), height());
    HomoMap homoV = HomoMap::Zero(width(), height());

    // interpolate green channel both horizontally and vertically
    rgbH.demosaicGreenHorizontal(*this, redOffset);
    rgbV.demosaicGreenVertical(*this, redOffset);

    // interpolate the red and blue using the green as a guide
    rgbH.demosaicRedBlueGreenGuidedLinear(redOffset);
    rgbV.demosaicRedBlueGreenGuidedLinear(redOffset);

    // Scale factor to push XYZ values to [0,1] range
    float scale = 1.0 / (maxCoeff().max() * cameraToXYZ.maxCoeff());

    // Precompute a table for the nonlinear part of the CIELab conversion
    vector<float> labLUT;
    labLUT.reserve(0xFFFF);
    parallel_for(0, labLUT.size(), [&labLUT](int i)
    {
        float r = i * 1.0f / (labLUT.size()-1);
        labLUT[i] = r > 0.008856 ? std::pow(r, 1.0f / 3.0f) : 7.787f*r + 4.0f/29.0f;
    });

    // convert both interpolated images to CIE L*a*b* so we can compute perceptual differences
    parallel_for(0, height(), [&rgbH,&labH,&cameraToXYZ,&labLUT,scale](int y)
    {
        for (int x = 0; x < rgbH.width(); ++x)
            labH(x,y) = cameraToLab(Vector3f(rgbH(x,y)[0], rgbH(x,y)[1], rgbH(x,y)[2])*scale, cameraToXYZ, labLUT);
    });
    parallel_for(0, height(), [&rgbV,&labV,&cameraToXYZ,&labLUT,scale](int y)
    {
        for (int x = 0; x < rgbV.width(); ++x)
            labV(x,y) = cameraToLab(Vector3f(rgbV(x,y)[0], rgbV(x,y)[1], rgbV(x,y)[2])*scale, cameraToXYZ, labLUT);
    });

    // Build homogeneity maps from the CIELab images which count, for each pixel,
    // the number of visually similar neighboring pixels
    static const int neighbor[4][2] = { {-1, 0}, {1, 0}, {0, -1}, {0, 1} };
    parallel_for(1, height()-1, [&homoH,&homoV,&labH,&labV](int y)
    {
        for (int x = 1; x < labH.rows()-1; ++x)
        {
            float ldiffH[4], ldiffV[4], abdiffH[4], abdiffV[4];

            for (int i = 0; i < 4; i++)
            {
                int dx = neighbor[i][0];
                int dy = neighbor[i][1];

                // Local luminance and chromaticity differences to the 4 neighbors for both interpolations directions
                ldiffH[i] = std::abs(labH(x,y)[0] - labH(x+dx,y+dy)[0]);
                ldiffV[i] = std::abs(labV(x,y)[0] - labV(x+dx,y+dy)[0]);
                abdiffH[i] = ::square(labH(x,y)[1] - labH(x+dx,y+dy)[1]) +
                             ::square(labH(x,y)[2] - labH(x+dx,y+dy)[2]);
                abdiffV[i] = ::square(labV(x,y)[1] - labV(x+dx,y+dy)[1]) +
                             ::square(labV(x,y)[2] - labV(x+dx,y+dy)[2]);
            }

            float leps = std::min(std::max(ldiffH[0], ldiffH[1]),
                                  std::max(ldiffV[2], ldiffV[3]));
            float abeps = std::min(std::max(abdiffH[0], abdiffH[1]),
                                   std::max(abdiffV[2], abdiffV[3]));

            // Count number of neighboring pixels that are visually similar
            for (int i = 0; i < 4; i++)
            {
                if (ldiffH[i] <= leps && abdiffH[i] <= abeps)
                    homoH(x,y)++;
                if (ldiffV[i] <= leps && abdiffV[i] <= abeps)
                    homoV(x,y)++;
            }
        }
    });

    // Combine the most homogenous pixels for the final result
    parallel_for(1, height()-1, [this,&homoH,&homoV,&rgbH,&rgbV](int y)
    {
        for (int x = 1; x < this->width()-1; ++x)
        {
            // Sum up the homogeneity of both images in a 3x3 window
            int hmH = 0, hmV = 0;
            for (int j = y-1; j <= y+1; j++)
                for (int i = x-1; i <= x+1; i++)
                {
                    hmH += homoH(i, j);
                    hmV += homoV(i, j);
                }

            if (hmH > hmV)
            {
                // horizontal interpolation is more homogeneous
                (*this)(x,y) = rgbH(x,y);
            }
            else if (hmV > hmH)
            {
                // vertical interpolation is more homogeneous
                (*this)(x,y) = rgbV(x,y);
            }
            else
            {
                // No clear winner, blend
                Color4 blend = (rgbH(x,y) + rgbV(x,y)) * 0.5f;
                (*this)(x,y) = blend;
            }
        }
    });

    // Now handle the boundary pixels
    demosaicBorder(3);
}

/*!
 * \brief   Demosaic the border of the image using naive averaging.
 *
 * Provides a results for all border pixels using a straight averge of the available pixels
 * in the 3x3 neighborhood. Useful in combination with more sophisticated methods which
 * require a larger window, and therefore cannot produce results at the image boundary.
 *
 * @param border    The size of the border in pixels.
 */
void HDRImage::demosaicBorder(size_t border)
{
    parallel_for(0, height(), [&](size_t y)
    {
        for (size_t x = 0; x < (size_t)width(); ++x)
        {
            // skip the center of the image
            if (x == border && y >= border && y < height() - border)
                x = width() - border;

            Vector3f sum = Vector3f::Zero();
            Vector3i count = Vector3i::Zero();

            for (size_t ys = y - 1; ys <= y + 1; ++ys)
            {
                for (size_t xs = x - 1; xs <= x + 1; ++xs)
                {
                    // rely on xs and ys = -1 to wrap around to max value
                    // since they are unsigned
                    if (ys < (size_t)height() && xs < (size_t)width())
                    {
                        int c = bayerColor(xs, ys);
                        sum(c) += (*this)(xs,ys)[c];
                        ++count(c);
                    }
                }
            }

            int col = bayerColor(x, y);
            for (int c = 0; c < 3; ++c)
            {
                if (col != c)
                    (*this)(x,y)[c] = count(c) ? (sum(c) / count(c)) : 1.0f;
            }
        }
    });
}



// local functions
namespace
{

// create a vector containing the normalized values of a 1D Gaussian filter
ArrayXXf horizontalGaussianKernel(float sigma, float truncate)
{
    // calculate the size of the filter
    int offset = int(std::ceil(truncate * sigma));
    int filterSize = 2*offset+1;

    ArrayXXf fData(filterSize, 1);

    // compute the un-normalized value of the Gaussian
    float normalizer = 0.0f;
    for (int i = 0; i < filterSize; i++)
    {
        fData(i,0) = std::exp(-pow(i - offset, 2) / (2.0f * pow(sigma, 2)));
        normalizer += fData(i,0);
    }

    // normalize
    for (int i = 0; i < filterSize; i++)
        fData(i,0) /= normalizer;

    return fData;
}

int wrapCoord(int p, int maxP, HDRImage::BorderMode m)
{
    if (p >= 0 && p < maxP)
        return p;

    switch (m)
    {
        case HDRImage::EDGE:
            return clamp(p, 0, maxP - 1);
        case HDRImage::REPEAT:
            return mod(p, maxP);
        case HDRImage::MIRROR:
        {
            int frac = mod(p, maxP);
            return (::abs(p) / maxP % 2 != 0) ? maxP - 1 - frac : frac;
        }
        case HDRImage::BLACK:
            return -1;
    }
}

inline Vector3f cameraToLab(const Vector3f c, const Matrix3f & cameraToXYZ, const vector<float> & LUT)
{
    Vector3f xyz = cameraToXYZ * c;

    for (int i = 0; i < 3; ++i)
        xyz(i) = LUT[clamp((int) (xyz(i) * LUT.size()), 0, int(LUT.size()-1))];

    return Vector3f(116.0f * xyz[1] - 16, 500.0f * (xyz[0] - xyz[1]), 200.0f * (xyz[1] - xyz[2]));
}

inline int bayerColor(int x, int y)
{
    const int bayer[2][2] = {{0,1},{1,2}};

    return bayer[y % 2][x % 2];
}

inline float clamp2(float value, float mn, float mx)
{
    if (mn > mx)
        std::swap(mn, mx);
    return clamp(value, mn, mx);
}

inline float clamp4(float value, float a, float b, float c, float d)
{
    float mn = min(a, b, c, d);
    float mx = max(a, b, c, d);
    return clamp(value, mn, mx);
}

inline float interpGreenH(const HDRImage &raw, int x, int y)
{
    float v = 0.50f * (raw(x - 1, y).g + raw(x + 1, y).g + raw(x, y).g) -
              0.25f * (raw(x - 2, y).g + raw(x + 2, y).g);
    // Don't extrapolate past the neighboring green values
    return clamp2(v, raw(x - 1, y).g, raw(x + 1, y).g);
}

inline float interpGreenV(const HDRImage &raw, int x, int y)
{
    float v = 0.50f * (raw(x, y - 1).g + raw(x, y + 1).g + raw(x, y).g) -
              0.25f * (raw(x, y - 2).g + raw(x, y + 2).g);
    // Don't extrapolate past the neighboring green values
    return clamp2(v, raw(x, y - 1).g, raw(x, y + 1).g);
}

inline float ghG(const ArrayXXf & G, int i, int j)
{
    return fabs(G(i-1,j) - G(i,j)) + fabs(G(i+1,j) - G(i,j));
}

inline float gvG(const ArrayXXf & G, int i, int j)
{
    return fabs(G(i,j-1) - G(i,j)) + fabs(G(i,j+1) - G(i,j));
}

void bilinearGreen(HDRImage &raw, int offsetX, int offsetY)
{
    parallel_for(1, raw.height()-1-offsetY, 2, [&raw,offsetX,offsetY](int yy)
    {
        int t = yy + offsetY;
        for (int xx = 1; xx < raw.width()-1-offsetX; xx += 2)
        {
            int l = xx + offsetX;

            // coordinates of the missing green pixels (red and blue) in
            // this Bayer tile are: (l,t) and (r,b)
            int r = l+1;
            int b = t+1;

            raw(l, t).g = 0.25f * (raw(l, t - 1).g + raw(l, t + 1).g +
                                   raw(l - 1, t).g + raw(l + 1, t).g);
            raw(r, b).g = 0.25f * (raw(r, b - 1).g + raw(r, b + 1).g +
                                   raw(r - 1, b).g + raw(r + 1, b).g);
        }
    });
}


void PhelippeauGreen(HDRImage &raw, const Vector2i & redOffset)
{
    ArrayXXf Gh(raw.width(), raw.height());
    ArrayXXf Gv(raw.width(), raw.height());

    // populate horizontally interpolated green
    parallel_for(redOffset.y(), raw.height(), 2, [&raw,&Gh,&redOffset](int y)
    {
        for (int x = 2+redOffset.x(); x < raw.width() - 2; x += 2)
        {
            Gh(x  , y  ) = interpGreenH(raw, x, y);
            Gh(x+1, y+1) = interpGreenH(raw, x + 1, y + 1);
        }
    });

    // populate vertically interpolated green
    parallel_for(2+redOffset.y(), raw.height()-2, 2, [&raw,&Gv,&redOffset](int y)
    {
        for (int x = redOffset.x(); x < raw.width(); x += 2)
        {
            Gv(x  , y  ) = interpGreenV(raw, x, y);
            Gv(x+1, y+1) = interpGreenV(raw, x + 1, y + 1);
        }
    });

    parallel_for(2+redOffset.y(), raw.height()-2, 2, [&raw,&Gh,&Gv,redOffset](int y)
    {
        for (int x = 2+redOffset.x(); x < raw.width()-2; x += 2)
        {
            float ghGh = ghG(Gh, x, y);
            float ghGv = ghG(Gv, x, y);
            float gvGh = gvG(Gh, x, y);
            float gvGv = gvG(Gv, x, y);

            raw(x, y).g = (ghGh + gvGh <= gvGv + ghGv) ? Gh(x, y) : Gv(x, y);

            x++;
            y++;

            ghGh = ghG(Gh, x, y);
            ghGv = ghG(Gv, x, y);
            gvGh = gvG(Gh, x, y);
            gvGv = gvG(Gv, x, y);

            raw(x, y).g = (ghGh + gvGh <= gvGv + ghGv) ? Gh(x, y) : Gv(x, y);
        }
    });
}


void MalvarGreen(HDRImage &raw, int c, const Vector2i & redOffset)
{
    // fill in half of the missing locations (R or B)
    parallel_for(2, raw.height()-2-redOffset.y(), 2, [&raw,c,&redOffset](int yy)
    {
        int y = yy + redOffset.y();
        for (int xx = 2; xx < raw.width()-2-redOffset.x(); xx += 2)
        {
            int x = xx + redOffset.x();
            float v = (4.f * raw(x,y)[c]
                         + 2.f * (raw(x, y-1)[1] + raw(x-1, y)[1] +
                                  raw(x, y+1)[1] + raw(x+1, y)[1])
                         - 1.f * (raw(x, y-2)[c] + raw(x-2, y)[c] +
                                  raw(x, y+2)[c] + raw(x+2, y)[c]))/8.f;
            raw(x,y)[1] = clamp4(v, raw(x, y-1)[1], raw(x-1, y)[1],
                                    raw(x, y+1)[1], raw(x+1, y)[1]);
        }
    });
}

void MalvarRedOrBlueAtGreen(HDRImage &raw, int c, const Vector2i &redOffset, bool horizontal)
{
    int dx = (horizontal) ? 1 : 0;
    int dy = (horizontal) ? 0 : 1;
    // fill in half of the missing locations (R or B)
    parallel_for(2+redOffset.y(), raw.height()-2, 2, [&raw,c,&redOffset,dx,dy](int y)
    {
        for (int x = 2+redOffset.x(); x < raw.width()-2; x += 2)
        {
            raw(x,y)[c] = (5.f * raw(x,y)[1]
                         - 1.f * (raw(x-1,y-1)[1] + raw(x+1,y-1)[1] + raw(x+1,y+1)[1] + raw(x-1,y+1)[1] + raw(x-2,y)[1] + raw(x+2,y)[1])
                         + .5f * (raw(x,y-2)[1] + raw(x,y+2)[1])
                         + 4.f * (raw(x-dx,y-dy)[c] + raw(x+dx,y+dy)[c]))/8.f;
        }
    });
}

void MalvarRedOrBlue(HDRImage &raw, int c1, int c2, const Vector2i &redOffset)
{
    // fill in half of the missing locations (R or B)
    parallel_for(2+redOffset.y(), raw.height()-2, 2, [&raw,c1,c2,&redOffset](int y)
    {
        for (int x = 2+redOffset.x(); x < raw.width()-2; x += 2)
        {
            raw(x,y)[c1] = (6.f * raw(x,y)[c2]
                          + 2.f * (raw(x-1,y-1)[c1] + raw(x+1,y-1)[c1] + raw(x+1,y+1)[c1] + raw(x-1,y+1)[c1])
                          - 3/2.f * (raw(x,y-2)[c2] + raw(x,y+2)[c2] + raw(x-2,y)[c2] + raw(x+2,y)[c2]))/8.f;
        }
    });
}


// takes as input a raw image and returns a single-channel
// 2D image corresponding to the red or blue channel using simple interpolation
void bilinearRedBlue(HDRImage &raw, int c, const Vector2i & redOffset)
{
    // diagonal interpolation
    parallel_for(redOffset.y() + 1, raw.height()-1, 2, [&raw,c,&redOffset](int y)
    {
        for (int x = redOffset.x() + 1; x < raw.width() - 1; x += 2)
            raw(x, y)[c] = 0.25f * (raw(x - 1, y - 1)[c] + raw(x + 1, y - 1)[c] +
                                    raw(x - 1, y + 1)[c] + raw(x + 1, y + 1)[c]);
    });

    // horizontal interpolation
    parallel_for(redOffset.y(), raw.height(), 2, [&raw,c,&redOffset](int y)
    {
        for (int x = redOffset.x() + 1; x < raw.width() - 1; x += 2)
            raw(x, y)[c] = 0.5f * (raw(x - 1, y)[c] + raw(x + 1, y)[c]);
    });

    // vertical interpolation
    parallel_for(redOffset.y() + 1, raw.height() - 1, 2, [&raw,c,&redOffset](int y)
    {
        for (int x = redOffset.x(); x < raw.width(); x += 2)
            raw(x, y)[c] = 0.5f * (raw(x, y - 1)[c] + raw(x, y + 1)[c]);
    });
}


// takes as input a raw image and returns a single-channel
// 2D image corresponding to the red or blue channel using green based interpolation
void greenBasedRorB(HDRImage &raw, int c, const Vector2i &redOffset)
{
    // horizontal interpolation
    parallel_for(redOffset.y(), raw.height(), 2, [&raw,c,&redOffset](int y)
    {
        for (int x = redOffset.x() + 1; x < raw.width() - 1; x += 2)
            raw(x, y)[c] = std::max(0.f, 0.5f * (raw(x - 1, y)[c] + raw(x + 1, y)[c] -
                                                 raw(x - 1, y)[1] - raw(x + 1, y)[1]) + raw(x, y)[1]);
    });

    // vertical interpolation
    parallel_for(redOffset.y() + 1, raw.height() - 1, 2, [&raw,c,&redOffset](int y)
    {
        for (int x = redOffset.x(); x < raw.width(); x += 2)
            raw(x, y)[c] = std::max(0.f, 0.5f * (raw(x, y - 1)[c] + raw(x, y + 1)[c] -
                                                 raw(x, y - 1)[1] - raw(x, y + 1)[1]) + raw(x, y)[1]);
    });

    // diagonal interpolation
    parallel_for(redOffset.y() + 1, raw.height() - 1, 2, [&raw,c,&redOffset](int y)
    {
        for (int x = redOffset.x() + 1; x < raw.width() - 1; x += 2)
            raw(x, y)[c] = std::max(0.f, 0.25f * (raw(x - 1, y - 1)[c] + raw(x + 1, y - 1)[c] +
                                                  raw(x - 1, y + 1)[c] + raw(x + 1, y + 1)[c] -
                                                  raw(x - 1, y - 1)[1] - raw(x + 1, y - 1)[1] -
                                                  raw(x - 1, y + 1)[1] - raw(x + 1, y + 1)[1]) + raw(x, y)[1]);
    });
}

} // namespace
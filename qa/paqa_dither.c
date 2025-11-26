/** @file paqa_dither.c
    @ingroup qa_src
    @brief Tests the dither scaling and conversion accuracy in pa_converters.c
    @author Phil Burk <philburk@mobileer.com>

    Link with pa_dither.c and pa_converters.c
*/
/*
 * $Id: $
 *
 * This program uses the PortAudio Portable Audio Library.
 * For more information see: http://www.portaudio.com/
 * Copyright (c) 1999-2008 Ross Bencina and Phil Burk
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
 * CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * The text above constitutes the entire PortAudio license; however,
 * the PortAudio community also makes the following non-binding requests:
 *
 * Any person wishing to distribute modifications to the Software is
 * requested to send the modifications to the original developer so that
 * they can be incorporated into the canonical version. It is also
 * requested that these non-binding requests be included along with the
 * license above.
 */
#include <stdio.h>
#include <stdlib.h> /* for EXIT_SUCCESS and EXIT_FAILURE */
#include <string.h>
#include <math.h>

#include "portaudio.h"
#include "pa_converters.h"
#include "pa_dither.h"
#include "pa_types.h"
#include "pa_endianness.h"
#include "paqa_macros.h"

PAQA_INSTANTIATE_GLOBALS

#define PAQA_SHOW_CHARTS 0

/**
 * Overrange values will be marked with "[" or "]".
 * @param numStars value between 0 to 100
 */
static void printStars(int numStars) {
    static char *stars = "****************************************" /* 40 */
            "****************************************" /* 40 */
            "********************"; /* 20 */
    if (numStars < 0) {
        printf("[\n");
    } else {
        if (numStars > 100) {
            printf("%.*s]\n", 99, stars);
        } else {
            printf("%.*s\n", numStars, stars);
        }
    }
}

// copied here for now otherwise we need to include the world just for this function.
static PaError MyPa_GetFormatSize( PaSampleFormat format )
{
    int result;

    switch( format & ~paNonInterleaved )
    {

    case paUInt8:
    case paInt8:
        result = 1;
        break;

    case paInt16:
        result = 2;
        break;

    case paInt24:
        result = 3;
        break;

    case paFloat32:
    case paInt32:
        result = 4;
        break;

    default:
        result = paSampleFormatNotSupported;
        break;
    }

    return (PaError) result;
}

static char * MyPa_GetFormatName( PaSampleFormat format )
{
    char * result = "?";
    switch( format & ~paNonInterleaved )
    {
        case paUInt8:
            result = "paUint8";
            break;
        case paInt8:
            result = "paInt8";
            break;
        case paInt16:
            result = "paInt16";
            break;
        case paInt24:
            result = "paInt24";
            break;
        case paInt32:
            result = "paInt32";
            break;
        case paFloat32:
            result = "paFloat32";
            break;
        default:
            result = "UNDEFINED";
            break;
    }
    return result;
}

#define NUM_BINS 32
/**
 * Show a histogram of the dither values.
 */
int ShowDitherDistribution( void )
{
    PaUtilTriangularDitherGenerator ditherGenerator;
    PaUtil_InitializeTriangularDitherState( &ditherGenerator );
    const int kNumSamples = 24 * 1024;
    PaInt32 minDither = 0x01000000;
    PaInt32 maxDither = -0x01000000;
    int histogram[NUM_BINS];
    int maxCount = 0;

    for (int i = 0; i < NUM_BINS; i++) {
        histogram[i] = 0;
    }

    printf("======= 16-bit dither distribution ===================\n");
    for (int i = 0; i < kNumSamples; i++) {
        PaInt32 dither = PaUtil_Generate16BitTriangularDither( &ditherGenerator );
        int binIndex = (dither * NUM_BINS >> 16) + (NUM_BINS / 2);
        if (binIndex < 0 || binIndex > (NUM_BINS - 1)) {
            printf("ERROR binIndex = %d, dither = %d\n", binIndex, dither);
        } else {
            histogram[binIndex]++;
        }
        minDither = (dither < minDither) ? dither : minDither;
        maxDither = (dither > maxDither) ? dither : maxDither;
    }
    for (int i = 0; i < NUM_BINS; i++) {
        maxCount = (histogram[i] > maxCount) ? histogram[i] : maxCount;
    }
    for (int i = 0; i < NUM_BINS; i++) {
        PaInt32 dither = ((i - (NUM_BINS / 2)) << 16) / NUM_BINS;
        printf("%6d, %4d, ", dither, histogram[i]);
        int numStars = histogram[i] * 100 / maxCount;
        printStars(numStars);
    }
    printf("minDither = %d, maxDither = %d\n\n", minDither, maxDither);
    return 0;
}

#define NUM_SAMPLES 1024
static double MeasureAverageConversion(PaSampleFormat sourceFormat,
                                       PaSampleFormat destinationFormat,
                                       double targetValue
                              ) {
    char source[NUM_SAMPLES * sizeof(PaInt32)];
    char destination[NUM_SAMPLES * sizeof(PaInt32)];
    PaUtilTriangularDitherGenerator ditherState;
    PaUtilConverter *converter;
    double sourceValue;
    if (sourceFormat == paFloat32) {
        sourceValue = targetValue / (1 << ((8 * MyPa_GetFormatSize(destinationFormat)) - 1));
    } else { // integer PCM
        int shift = 8 * (MyPa_GetFormatSize(sourceFormat) - MyPa_GetFormatSize(destinationFormat));
        sourceValue = targetValue * (1 << shift);
    }

    PaUtil_InitializeTriangularDitherState( &ditherState );

    switch( sourceFormat ) {
        case paFloat32:
            for ( int i = 0; i < NUM_SAMPLES; i++ ) ((float *)source)[i] = (float)sourceValue;
            break;
        case paInt32:
            for ( int i = 0; i < NUM_SAMPLES; i++ ) ((PaInt32 *)source)[i] = (PaInt32)sourceValue;
            break;
        case paInt16:
            for ( int i = 0; i < NUM_SAMPLES; i++ ) ((PaInt16 *)source)[i] = (PaInt16)sourceValue;
            break;
    }
    memset(destination, 0, sizeof(destination));

    converter = PaUtil_SelectConverter( sourceFormat, destinationFormat, paClipOff ); // paClipOff or paNoFlag
    (*converter)( destination, 1, source, 1, NUM_SAMPLES, &ditherState );
    double sum = 0.0f;
    switch( destinationFormat ) {
        case paInt16:
            for ( int i = 0; i < NUM_SAMPLES; i++ ) sum += (double) ((PaInt16 *)destination)[i];
            break;
        case paInt8:
            for ( int i = 0; i < NUM_SAMPLES; i++ ) sum += (double) ((signed char *)destination)[i];
            break;
    }

    return sum / NUM_SAMPLES;
}

/**
 * Calculate the Coefficient of Determination, "R-squared".
 * You want a value as close to 1.0 as possible.
 */
double CalculateRSquared(double *xa, double *ya, int numPoints) {
    double sum_squares_residual = 0;
    double sum_squares_total = 0;
    double meanY;
    double sumY = 0;
    for (int i = 0; i < numPoints; i++) {
        sumY += ya[i];
    }
    meanY = sumY / numPoints;

    for (int i = 0; i < numPoints; i++) {
        sum_squares_residual += (ya[i] - xa[i]) * (ya[i] - xa[i]);
        sum_squares_total += (ya[i] - meanY) * (ya[i] - meanY);
    }

    if (sum_squares_total == 0) {
        return 1.0; // if total sum of squares is zero, the model explains all the variance
    }

    return 1.0 - (sum_squares_residual / sum_squares_total);
}

// Function to calculate the linear regression parameters
void linearRegression(double *xa, double *ya, int numPoints, double *a, double *b) {
    double sumX = 0, sumY = 0, sumXY = 0, sumX2 = 0;

    for (int i = 0; i < numPoints; i++) {
        sumX += xa[i];
        sumY += ya[i];
        sumXY += xa[i] * ya[i];
        sumX2 += xa[i] * xa[i];
    }

    *a = (numPoints * sumXY - sumX * sumY) / (numPoints * sumX2 - sumX * sumX);
    *b = (sumY - (*a) * sumX) / numPoints;
}

#define LINEARITY_NUM_STEPS 41
static int TestDitherLinearity(PaSampleFormat sourceFormat, PaSampleFormat destinationFormat) {
    int result = 0;
    double minValue = -2.0;
    double maxValue = 2.0;
    double averages[LINEARITY_NUM_STEPS];
    double expected[LINEARITY_NUM_STEPS];
    double stride = (maxValue - minValue) / (LINEARITY_NUM_STEPS - 1);
    char supported = 0;
    double slope = 0.0;
    double bias = 0.0;
    double rSquared;

    printf(" ============= Linearity: %9s => %7s ============== \n",
           MyPa_GetFormatName(sourceFormat), MyPa_GetFormatName(destinationFormat));

    for (int i = 0; i < LINEARITY_NUM_STEPS; i++) {
        double destinationValue = minValue + (i * stride);
        expected[i] = destinationValue;
        averages[i] = MeasureAverageConversion(sourceFormat, destinationFormat, destinationValue);
        if (averages[i] != 0.0) {
            supported = 1;
        }
    }
    ASSERT_TRUE(supported);

    linearRegression(expected, averages, LINEARITY_NUM_STEPS, &slope, &bias);
    rSquared = CalculateRSquared(expected, averages, LINEARITY_NUM_STEPS);
    printf("slope = %f, bias = %f, rSquared = %f\n", slope, bias, rSquared);
    EXPECT_TRUE((slope < 1.02));
    EXPECT_TRUE((slope > 0.98));
    EXPECT_TRUE((bias > -0.01));
    EXPECT_TRUE((bias < 0.01));
    EXPECT_TRUE((rSquared > 0.99));

#if PAQA_SHOW_CHARTS
    for (int i = 0; i < LINEARITY_NUM_STEPS; i++) {
        printf("%8.5f => %8.5f: ", expected[i], averages[i]);
        int numStars = 2 * (int)((averages[i] - minValue) / stride);
        printStars(numStars);
    }
#endif

error:
    return result;
}


int TestAllDitherScaling( void )
{
    TestDitherLinearity(paFloat32, paInt16);
    TestDitherLinearity(paFloat32, paInt8);
    TestDitherLinearity(paFloat32, paInt8);
    TestDitherLinearity(paInt32, paInt16);
    TestDitherLinearity(paInt32, paInt8);
    TestDitherLinearity(paInt16, paInt8);
    return 0;
}

/**
 * Check to see whether the dithering causes a numeric wraparound.
 */
static int TestDitherClippingSingle(PaSampleFormat sourceFormat,
                                    PaSampleFormat destinationFormat,
                                    double sourceValue,
                                    PaStreamFlags streamFlags) {
    char source[NUM_SAMPLES * sizeof(PaInt32)];
    char destination[NUM_SAMPLES * sizeof(PaInt32)];
    PaUtilTriangularDitherGenerator ditherState;
    PaUtilConverter *converter;
    char supported = 0;

    PaUtil_InitializeTriangularDitherState( &ditherState );
    switch( sourceFormat ) {
        case paFloat32:
            for ( int i = 0; i < NUM_SAMPLES; i++ ) ((float *)source)[i] = (float)sourceValue;
            break;
        case paInt32:
            for ( int i = 0; i < NUM_SAMPLES; i++ ) ((PaInt32 *)source)[i] = (PaInt32)sourceValue;
            break;
        case paInt16:
            for ( int i = 0; i < NUM_SAMPLES; i++ ) ((PaInt16 *)source)[i] = (PaInt16)sourceValue;
            break;
    }
    memset(destination, 0, sizeof(destination));

    converter = PaUtil_SelectConverter( sourceFormat, destinationFormat, streamFlags );
    (*converter)( destination, 1, source, 1, NUM_SAMPLES, &ditherState );
    /* Try to detect wrapping, which causes a huge delta. */
    int previousValue = 0;
    int maxDelta = 0;
    switch( destinationFormat ) {
        case paInt16:
            previousValue = ((PaInt16 *)destination)[0];
            for ( int i = 1; i < NUM_SAMPLES; i++ ) {
                int value = (int) ((PaInt16 *)destination)[i];
                int delta = abs(value - previousValue);
                if (delta > maxDelta) {
                    maxDelta = delta;
                }
                if (value != 0) {
                    supported = 1;
                }
            }
            break;
        case paInt8:
            previousValue = ((signed char *)destination)[0];
            for ( int i = 1; i < NUM_SAMPLES; i++ ) {
                int value = (int) ((signed char *)destination)[i];
                int delta = abs(value - previousValue);
                if (delta > maxDelta) {
                    maxDelta = delta;
                }
                if (value != 0) {
                    supported = 1;
                }
            }
            break;
        case paUInt8:
            previousValue = ((unsigned char *)destination)[0];
            for ( int i = 1; i < NUM_SAMPLES; i++ ) {
                int value = (int) ((unsigned char *)destination)[i];
                int delta = abs(value - previousValue);
                if (delta > maxDelta) {
                    maxDelta = delta;
                }
                if (value != 128) {
                    supported = 1;
                }
            }
            break;
    }

    ASSERT_TRUE(supported);
    ASSERT_LT(maxDelta, 2);
error:
    return maxDelta;
}


static int TestDitherClipping(PaSampleFormat sourceFormat, PaSampleFormat destinationFormat) {
    int result = 0;

    printf(" ============= Clipping: %9s => %7s ============== \n",
           MyPa_GetFormatName(sourceFormat), MyPa_GetFormatName(destinationFormat));
    double minSourceValue;
    double maxSourceValue;
    switch (sourceFormat) {
        case paFloat32:
            maxSourceValue = 0.999999;
            minSourceValue = -1.0;
            break;
        case paInt32:
            maxSourceValue = (double)(0x7FFFFFFF);
            minSourceValue = -(double)(0x80000000);
            break;
        case paInt16:
            maxSourceValue = (double)((1 << 15) - 1);
            minSourceValue = -(double)(1 << 15);
            break;
        default:
            maxSourceValue = 0.0;
            minSourceValue = 0.0;
            break;
    }
    TestDitherClippingSingle(sourceFormat, destinationFormat, minSourceValue, paNoFlag);
    TestDitherClippingSingle(sourceFormat, destinationFormat, maxSourceValue, paNoFlag);
    TestDitherClippingSingle(sourceFormat, destinationFormat, minSourceValue, paClipOff);
    TestDitherClippingSingle(sourceFormat, destinationFormat, maxSourceValue, paClipOff);
    return result;
}


int TestAllDitherClipping( void )
{
    TestDitherClipping(paFloat32, paInt16);
    TestDitherClipping(paFloat32, paInt8);
    TestDitherClipping(paFloat32, paUInt8);
    // TODO support 24-bit
    TestDitherClipping(paInt32, paInt16);
    TestDitherClipping(paInt32, paInt8);
    TestDitherClipping(paInt32, paUInt8);
    TestDitherClipping(paInt16, paInt8);
    TestDitherClipping(paInt16, paUInt8);
    return 0;
}

int main( int argc, const char **argv )
{
    ShowDitherDistribution();
    TestAllDitherScaling();
    TestAllDitherClipping();

    PAQA_PRINT_RESULT;
    return PAQA_EXIT_RESULT;
}

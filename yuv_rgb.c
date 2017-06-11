// Copyright 2016 Adrien Descamps
// Distributed under BSD 3-Clause License

#include "yuv_rgb.h"

#include <x86intrin.h>

#include <stdio.h>

uint8_t clamp(int16_t value)
{
	return value<0 ? 0 : (value>255 ? 255 : value);
}

// Definitions
//
// E'R, E'G, E'B, E'Y, E'Cb and E'Cr refer to the analog signals
// E'R, E'G, E'B and E'Y range is [0:1], while E'Cb and E'Cr range is [-0.5:0.5]
// R, G, B, Y, Cb and Cr refer to the digitalized values
// The digitalized values can use their full range ([0:255] for 8bit values),
// or a subrange (typically [16:235] for Y and [16:240] for CbCr).
// We assume here that RGB range is always [0:255], since it is the case for 
// most digitalized images.
// For 8bit values :
// * Y = round((YMax-YMin)*E'Y + YMin)
// * Cb = round((CbRange)*E'Cb + 128)
// * Cr = round((CrRange)*E'Cr + 128)
// Where *Min and *Max are the range of each channel
//
// In the analog domain , the RGB to YCbCr transformation is defined as:
// * E'Y = Rf*E'R + Gf*E'G + Bf*E'B
// Where Rf, Gf and Bf are constants defined in each standard, with 
// Rf + Gf + Bf = 1 (necessary to ensure that E'Y range is [0:1])
// * E'Cb = (E'B - E'Y) / CbNorm
// * E'Cr = (E'R - E'Y) / CrNorm
// Where CbNorm and CrNorm are constants, dependent of Rf, Gf, Bf, computed 
// to normalize to a [-0.5:0.5] range : CbNorm=2*(1-Bf) and CrNorm=2*(1-Rf)
//
// Algorithms
//
// Most operations will be made in a fixed point format for speed, using 
// N bits of precision. In next section the [x] convention is used for 
// a fixed point rounded value, that is (int being the c type conversion)
// * [x] = int(x*(2^N)+0.5)
// Unless precised otherwise, we use N=7
//.
// For RGB to YCbCr conversion, we start by generating a pseudo Y value 
// (noted Y') in fixed point format, using the full range for now.
// * Y' = ([Rf]*R + [Gf]*G + [Bf]*B)>>N
// We can then compute Cb and Cr by
// * Cb = ((B - Y')*[CbRange/(255*CbNorm)])>>N + 128
// * Cr = ((R - Y')*[CrRange/(255*CrNorm)])>>N + 128
// And finally, we normalize Y to its digital range
// * Y = (Y'*[(YMax-YMin)/255])>>N + YMin
// 
// For YCbCr to RGB conversion, we first compute the full range Y' value :
// * Y' = ((Y-YMin)*[255/(YMax-YMin)])>>N
// We can then compute B and R values by :
// * B = ((Cb-128)*[(255*CbNorm)/CbRange])>>N + Y'
// * R = ((Cr-128)*[(255*CrNorm)/CrRange])>>N + Y'
// And finally, for G we know that:
// * G = (Y' - (Rf*R + Bf*B)) / Gf
// From above:
// * G = (Y' - Rf * ((Cr-128)*(255*CrNorm)/CrRange + Y') - Bf * ((Cb-128)*(255*CbNorm)/CbRange + Y')) / Gf
// Since 1-Rf-Bf=Gf, we can take Y' out of the division by Gf, and we get:
// * G = Y' - (Cr-128)*Rf/Gf*(255*CrNorm)/CrRange - (Cb-128)*Bf/Gf*(255*CbNorm)/CbRange
// That we can compute, with fixed point arithmetic, by
// * G = Y' - ((Cr-128)*[Rf/Gf*(255*CrNorm)/CrRange] + (Cb-128)*[Bf/Gf*(255*CbNorm)/CbRange])>>N
// 
// Note : in ITU-T T.871(JPEG), Y=Y', so that part could be optimized out


#define FIXED_POINT_VALUE(value, precision) ((int)(((value)*(1<<precision))+0.5))

// see above for description
typedef struct
{
	uint8_t r_factor;    // [Rf]
	uint8_t g_factor;    // [Rg]
	uint8_t b_factor;    // [Rb]
	uint8_t cb_factor;   // [CbRange/(255*CbNorm)]
	uint8_t cr_factor;   // [CrRange/(255*CrNorm)]
	uint8_t y_factor;    // [(YMax-YMin)/255]
	uint8_t y_offset;    // YMin
} RGB2YUVParam;

typedef struct
{
	uint8_t cb_factor;   // [(255*CbNorm)/CbRange]
	uint8_t cr_factor;   // [(255*CrNorm)/CrRange]
	uint8_t g_cb_factor; // [Bf/Gf*(255*CbNorm)/CbRange]
	uint8_t g_cr_factor; // [Rf/Gf*(255*CrNorm)/CrRange]
	uint8_t y_factor;    // [(YMax-YMin)/255]
	uint8_t y_offset;    // YMin
} YUV2RGBParam;

#define RGB2YUV_PARAM(Rf, Bf, YMin, YMax, CbCrRange) \
{.r_factor=FIXED_POINT_VALUE(Rf, 8), \
.g_factor=256-FIXED_POINT_VALUE(Rf, 8)-FIXED_POINT_VALUE(Bf, 8), \
.b_factor=FIXED_POINT_VALUE(Bf, 8), \
.cb_factor=FIXED_POINT_VALUE((CbCrRange/255.0)/(2.0*(1-Bf)), 8), \
.cr_factor=FIXED_POINT_VALUE((CbCrRange/255.0)/(2.0*(1-Rf)), 8), \
.y_factor=FIXED_POINT_VALUE((YMax-YMin)/255.0, 7), \
.y_offset=YMin}

#define YUV2RGB_PARAM(Rf, Bf, YMin, YMax, CbCrRange) \
{.cb_factor=FIXED_POINT_VALUE(255.0*(2.0*(1-Bf))/CbCrRange, 6), \
.cr_factor=FIXED_POINT_VALUE(255.0*(2.0*(1-Rf))/CbCrRange, 6), \
.g_cb_factor=FIXED_POINT_VALUE(Bf/(1.0-Bf-Rf)*255.0*(2.0*(1-Bf))/CbCrRange, 7), \
.g_cr_factor=FIXED_POINT_VALUE(Rf/(1.0-Bf-Rf)*255.0*(2.0*(1-Rf))/CbCrRange, 7), \
.y_factor=FIXED_POINT_VALUE(255.0/(YMax-YMin), 7), \
.y_offset=YMin}

static const RGB2YUVParam RGB2YUV[3] = {
	// ITU-T T.871 (JPEG)
	RGB2YUV_PARAM(0.299, 0.114, 0.0, 255.0, 255.0),
	// ITU-R BT.601-7
	RGB2YUV_PARAM(0.299, 0.114, 16.0, 235.0, 224.0),
	// ITU-R BT.709-6
	RGB2YUV_PARAM(0.2126, 0.0722, 16.0, 235.0, 224.0)
};

static const YUV2RGBParam YUV2RGB[3] = {
	// ITU-T T.871 (JPEG)
	YUV2RGB_PARAM(0.299, 0.114, 0.0, 255.0, 255.0),
	// ITU-R BT.601-7
	YUV2RGB_PARAM(0.299, 0.114, 16.0, 235.0, 224.0),
	// ITU-R BT.709-6
	YUV2RGB_PARAM(0.2126, 0.0722, 16.0, 235.0, 224.0)
};


void rgb24_yuv420_std(
	uint32_t width, uint32_t height, 
	const uint8_t *RGB, uint32_t RGB_stride, 
	uint8_t *Y, uint8_t *U, uint8_t *V, uint32_t Y_stride, uint32_t UV_stride, 
	YCbCrType yuv_type)
{
	const RGB2YUVParam *const param = &(RGB2YUV[yuv_type]);
	
	uint32_t x, y;
	for(y=0; y<(height-1); y+=2)
	{
		const uint8_t *rgb_ptr1=RGB+y*RGB_stride,
			*rgb_ptr2=RGB+(y+1)*RGB_stride;
		
		uint8_t *y_ptr1=Y+y*Y_stride,
			*y_ptr2=Y+(y+1)*Y_stride,
			*u_ptr=U+(y/2)*UV_stride,
			*v_ptr=V+(y/2)*UV_stride;
		
		for(x=0; x<(width-1); x+=2)
		{
			// compute yuv for the four pixels, u and v values are summed
			uint8_t y_tmp;
			uint16_t u_tmp, v_tmp;
			
			y_tmp = (param->r_factor*rgb_ptr1[0] + param->g_factor*rgb_ptr1[1] + param->b_factor*rgb_ptr1[2])>>8;
			u_tmp = (((rgb_ptr1[2]-y_tmp)*param->cb_factor)>>8) + 128;
			v_tmp = (((rgb_ptr1[0]-y_tmp)*param->cr_factor)>>8) + 128;
			y_ptr1[0]=((y_tmp*param->y_factor)>>7) + param->y_offset;
			
			y_tmp = (param->r_factor*rgb_ptr1[3] + param->g_factor*rgb_ptr1[4] + param->b_factor*rgb_ptr1[5])>>8;
			u_tmp += (((rgb_ptr1[5]-y_tmp)*param->cb_factor)>>8) + 128;
			v_tmp += (((rgb_ptr1[3]-y_tmp)*param->cr_factor)>>8) + 128;
			y_ptr1[1]=((y_tmp*param->y_factor)>>7) + param->y_offset;

			y_tmp = (param->r_factor*rgb_ptr2[0] + param->g_factor*rgb_ptr2[1] + param->b_factor*rgb_ptr2[2])>>8;
			u_tmp += (((rgb_ptr2[2]-y_tmp)*param->cb_factor)>>8) + 128;
			v_tmp += (((rgb_ptr2[0]-y_tmp)*param->cr_factor)>>8) + 128;
			y_ptr2[0]=((y_tmp*param->y_factor)>>7) + param->y_offset;
			
			y_tmp = (param->r_factor*rgb_ptr2[3] + param->g_factor*rgb_ptr2[4] + param->b_factor*rgb_ptr2[5])>>8;
			u_tmp += (((rgb_ptr2[5]-y_tmp)*param->cb_factor)>>8) + 128;
			v_tmp += (((rgb_ptr2[3]-y_tmp)*param->cr_factor)>>8) + 128;
			y_ptr2[1]=((y_tmp*param->y_factor)>>7) + param->y_offset;

			u_ptr[0] = u_tmp>>2;
			v_ptr[0] = v_tmp>>2;
			
			rgb_ptr1 += 6;
			rgb_ptr2 += 6;
			y_ptr1 += 2;
			y_ptr2 += 2;
			u_ptr += 1;
			v_ptr += 1;
		}
	}
}


void yuv420_rgb24_std(
	uint32_t width, uint32_t height, 
	const uint8_t *Y, const uint8_t *U, const uint8_t *V, uint32_t Y_stride, uint32_t UV_stride, 
	uint8_t *RGB, uint32_t RGB_stride, 
	YCbCrType yuv_type)
{
	const YUV2RGBParam *const param = &(YUV2RGB[yuv_type]);
	uint32_t x, y;
	for(y=0; y<(height-1); y+=2)
	{
		const uint8_t *y_ptr1=Y+y*Y_stride,
			*y_ptr2=Y+(y+1)*Y_stride,
			*u_ptr=U+(y/2)*UV_stride,
			*v_ptr=V+(y/2)*UV_stride;
		
		uint8_t *rgb_ptr1=RGB+y*RGB_stride,
			*rgb_ptr2=RGB+(y+1)*RGB_stride;
		
		for(x=0; x<(width-1); x+=2)
		{
			int8_t u_tmp, v_tmp;
			u_tmp = u_ptr[0]-128;
			v_tmp = v_ptr[0]-128;
			
			//compute Cb Cr color offsets, common to four pixels
			int16_t b_cb_offset, r_cr_offset, g_cbcr_offset;
			b_cb_offset = (param->cb_factor*u_tmp)>>6;
			r_cr_offset = (param->cr_factor*v_tmp)>>6;
			g_cbcr_offset = (param->g_cb_factor*u_tmp + param->g_cr_factor*v_tmp)>>7;
			
			int16_t y_tmp;
			y_tmp = (param->y_factor*(y_ptr1[0]-param->y_offset))>>7;
			rgb_ptr1[0] = clamp(y_tmp + r_cr_offset);
			rgb_ptr1[1] = clamp(y_tmp - g_cbcr_offset);
			rgb_ptr1[2] = clamp(y_tmp + b_cb_offset);
			
			y_tmp = (param->y_factor*(y_ptr1[1]-param->y_offset))>>7;
			rgb_ptr1[3] = clamp(y_tmp + r_cr_offset);
			rgb_ptr1[4] = clamp(y_tmp - g_cbcr_offset);
			rgb_ptr1[5] = clamp(y_tmp + b_cb_offset);
			
			y_tmp = (param->y_factor*(y_ptr2[0]-param->y_offset))>>7;
			rgb_ptr2[0] = clamp(y_tmp + r_cr_offset);
			rgb_ptr2[1] = clamp(y_tmp - g_cbcr_offset);
			rgb_ptr2[2] = clamp(y_tmp + b_cb_offset);
			
			y_tmp = (param->y_factor*(y_ptr2[1]-param->y_offset))>>7;
			rgb_ptr2[3] = clamp(y_tmp + r_cr_offset);
			rgb_ptr2[4] = clamp(y_tmp - g_cbcr_offset);
			rgb_ptr2[5] = clamp(y_tmp + b_cb_offset);
			
			rgb_ptr1 += 6;
			rgb_ptr2 += 6;
			y_ptr1 += 2;
			y_ptr2 += 2;
			u_ptr += 1;
			v_ptr += 1;
		}
	}
}

#ifdef NOTDEFINED
#ifdef __SSE2__

#define UV2RGB_16(U,V,R1,G1,B1,R2,G2,B2) \
	r_tmp = _mm_mullo_epi16(V, _mm_set1_epi16(param->v_r_factor)); \
	g_tmp = _mm_add_epi16( \
		_mm_mullo_epi16(U, _mm_set1_epi16(param->u_g_factor)), \
		_mm_mullo_epi16(V, _mm_set1_epi16(param->v_g_factor))); \
	b_tmp = _mm_mullo_epi16(U, _mm_set1_epi16(param->u_b_factor)); \
	R1 = _mm_unpacklo_epi16(r_tmp, r_tmp); \
	G1 = _mm_unpacklo_epi16(g_tmp, g_tmp); \
	B1 = _mm_unpacklo_epi16(b_tmp, b_tmp); \
	R2 = _mm_unpackhi_epi16(r_tmp, r_tmp); \
	G2 = _mm_unpackhi_epi16(g_tmp, g_tmp); \
	B2 = _mm_unpackhi_epi16(b_tmp, b_tmp); \

#define ADD_Y2RGB_16(Y1,Y2,R1,G1,B1,R2,G2,B2) \
	Y1 = _mm_mullo_epi16(_mm_sub_epi16(Y1, _mm_set1_epi16(param->y_shift)), _mm_set1_epi16(param->y_factor)); \
	Y2 = _mm_mullo_epi16(_mm_sub_epi16(Y2, _mm_set1_epi16(param->y_shift)), _mm_set1_epi16(param->y_factor)); \
	\
	R1 = _mm_srai_epi16(_mm_add_epi16(R1, Y1), PRECISION); \
	G1 = _mm_srai_epi16(_mm_add_epi16(G1, Y1), PRECISION); \
	B1 = _mm_srai_epi16(_mm_add_epi16(B1, Y1), PRECISION); \
	R2 = _mm_srai_epi16(_mm_add_epi16(R2, Y2), PRECISION); \
	G2 = _mm_srai_epi16(_mm_add_epi16(G2, Y2), PRECISION); \
	B2 = _mm_srai_epi16(_mm_add_epi16(B2, Y2), PRECISION); \

#define PACK_RGB24_32_STEP1(R1, R2, G1, G2, B1, B2, RGB1, RGB2, RGB3, RGB4, RGB5, RGB6) \
RGB1 = _mm_packus_epi16(_mm_and_si128(R1,_mm_set1_epi16(0xFF)), _mm_and_si128(R2,_mm_set1_epi16(0xFF))); \
RGB2 = _mm_packus_epi16(_mm_and_si128(G1,_mm_set1_epi16(0xFF)), _mm_and_si128(G2,_mm_set1_epi16(0xFF))); \
RGB3 = _mm_packus_epi16(_mm_and_si128(B1,_mm_set1_epi16(0xFF)), _mm_and_si128(B2,_mm_set1_epi16(0xFF))); \
RGB4 = _mm_packus_epi16(_mm_srli_epi16(R1,8), _mm_srli_epi16(R2,8)); \
RGB5 = _mm_packus_epi16(_mm_srli_epi16(G1,8), _mm_srli_epi16(G2,8)); \
RGB6 = _mm_packus_epi16(_mm_srli_epi16(B1,8), _mm_srli_epi16(B2,8)); \

#define PACK_RGB24_32_STEP2(R1, R2, G1, G2, B1, B2, RGB1, RGB2, RGB3, RGB4, RGB5, RGB6) \
R1 = _mm_packus_epi16(_mm_and_si128(RGB1,_mm_set1_epi16(0xFF)), _mm_and_si128(RGB2,_mm_set1_epi16(0xFF))); \
R2 = _mm_packus_epi16(_mm_and_si128(RGB3,_mm_set1_epi16(0xFF)), _mm_and_si128(RGB4,_mm_set1_epi16(0xFF))); \
G1 = _mm_packus_epi16(_mm_and_si128(RGB5,_mm_set1_epi16(0xFF)), _mm_and_si128(RGB6,_mm_set1_epi16(0xFF))); \
G2 = _mm_packus_epi16(_mm_srli_epi16(RGB1,8), _mm_srli_epi16(RGB2,8)); \
B1 = _mm_packus_epi16(_mm_srli_epi16(RGB3,8), _mm_srli_epi16(RGB4,8)); \
B2 = _mm_packus_epi16(_mm_srli_epi16(RGB5,8), _mm_srli_epi16(RGB6,8)); \

#define PACK_RGB24_32(R1, R2, G1, G2, B1, B2, RGB1, RGB2, RGB3, RGB4, RGB5, RGB6) \
PACK_RGB24_32_STEP1(R1, R2, G1, G2, B1, B2, RGB1, RGB2, RGB3, RGB4, RGB5, RGB6) \
PACK_RGB24_32_STEP2(R1, R2, G1, G2, B1, B2, RGB1, RGB2, RGB3, RGB4, RGB5, RGB6) \
PACK_RGB24_32_STEP1(R1, R2, G1, G2, B1, B2, RGB1, RGB2, RGB3, RGB4, RGB5, RGB6) \
PACK_RGB24_32_STEP2(R1, R2, G1, G2, B1, B2, RGB1, RGB2, RGB3, RGB4, RGB5, RGB6) \
PACK_RGB24_32_STEP1(R1, R2, G1, G2, B1, B2, RGB1, RGB2, RGB3, RGB4, RGB5, RGB6) \

#define YUV2RGB_32 \
	__m128i r_tmp, g_tmp, b_tmp; \
	__m128i r_16_1, g_16_1, b_16_1, r_16_2, g_16_2, b_16_2; \
	__m128i r_uv_16_1, g_uv_16_1, b_uv_16_1, r_uv_16_2, g_uv_16_2, b_uv_16_2; \
	__m128i y_16_1, y_16_2; \
	\
	__m128i u = LOAD_SI128((const __m128i*)(u_ptr)); \
	__m128i v = LOAD_SI128((const __m128i*)(v_ptr)); \
	\
	/* process first 16 pixels of first line */\
	__m128i u_16 = _mm_unpacklo_epi8(u, _mm_setzero_si128()); \
	__m128i v_16 = _mm_unpacklo_epi8(v, _mm_setzero_si128()); \
	u_16 = _mm_add_epi16(u_16, _mm_set1_epi16(-128)); \
	v_16 = _mm_add_epi16(v_16, _mm_set1_epi16(-128)); \
	\
	UV2RGB_16(u_16, v_16, r_16_1, g_16_1, b_16_1, r_16_2, g_16_2, b_16_2) \
	r_uv_16_1=r_16_1; g_uv_16_1=g_16_1; b_uv_16_1=b_16_1; \
	r_uv_16_2=r_16_2; g_uv_16_2=g_16_2; b_uv_16_2=b_16_2; \
	\
	__m128i y = LOAD_SI128((const __m128i*)(y_ptr1)); \
	y_16_1 = _mm_unpacklo_epi8(y, _mm_setzero_si128()); \
	y_16_2 = _mm_unpackhi_epi8(y, _mm_setzero_si128()); \
	\
	ADD_Y2RGB_16(y_16_1, y_16_2, r_16_1, g_16_1, b_16_1, r_16_2, g_16_2, b_16_2) \
	\
	__m128i r_8_11 = _mm_packus_epi16(r_16_1, r_16_2); \
	__m128i g_8_11 = _mm_packus_epi16(g_16_1, g_16_2); \
	__m128i b_8_11 = _mm_packus_epi16(b_16_1, b_16_2); \
	\
	/* process first 16 pixels of second line */\
	r_16_1=r_uv_16_1; g_16_1=g_uv_16_1; b_16_1=b_uv_16_1; \
	r_16_2=r_uv_16_2; g_16_2=g_uv_16_2; b_16_2=b_uv_16_2; \
	\
	y = LOAD_SI128((const __m128i*)(y_ptr2)); \
	y_16_1 = _mm_unpacklo_epi8(y, _mm_setzero_si128()); \
	y_16_2 = _mm_unpackhi_epi8(y, _mm_setzero_si128()); \
	\
	ADD_Y2RGB_16(y_16_1, y_16_2, r_16_1, g_16_1, b_16_1, r_16_2, g_16_2, b_16_2) \
	\
	__m128i r_8_21 = _mm_packus_epi16(r_16_1, r_16_2); \
	__m128i g_8_21 = _mm_packus_epi16(g_16_1, g_16_2); \
	__m128i b_8_21 = _mm_packus_epi16(b_16_1, b_16_2); \
	\
	/* process last 16 pixels of first line */\
	u_16 = _mm_unpackhi_epi8(u, _mm_setzero_si128()); \
	v_16 = _mm_unpackhi_epi8(v, _mm_setzero_si128()); \
	u_16 = _mm_add_epi16(u_16, _mm_set1_epi16(-128)); \
	v_16 = _mm_add_epi16(v_16, _mm_set1_epi16(-128)); \
	\
	UV2RGB_16(u_16, v_16, r_16_1, g_16_1, b_16_1, r_16_2, g_16_2, b_16_2) \
	r_uv_16_1=r_16_1; g_uv_16_1=g_16_1; b_uv_16_1=b_16_1; \
	r_uv_16_2=r_16_2; g_uv_16_2=g_16_2; b_uv_16_2=b_16_2; \
	\
	y = LOAD_SI128((const __m128i*)(y_ptr1+16)); \
	y_16_1 = _mm_unpacklo_epi8(y, _mm_setzero_si128()); \
	y_16_2 = _mm_unpackhi_epi8(y, _mm_setzero_si128()); \
	\
	ADD_Y2RGB_16(y_16_1, y_16_2, r_16_1, g_16_1, b_16_1, r_16_2, g_16_2, b_16_2) \
	\
	__m128i r_8_12 = _mm_packus_epi16(r_16_1, r_16_2); \
	__m128i g_8_12 = _mm_packus_epi16(g_16_1, g_16_2); \
	__m128i b_8_12 = _mm_packus_epi16(b_16_1, b_16_2); \
	\
	/* process last 16 pixels of second line */\
	r_16_1=r_uv_16_1; g_16_1=g_uv_16_1; b_16_1=b_uv_16_1; \
	r_16_2=r_uv_16_2; g_16_2=g_uv_16_2; b_16_2=b_uv_16_2; \
	\
	y = LOAD_SI128((const __m128i*)(y_ptr2+16)); \
	y_16_1 = _mm_unpacklo_epi8(y, _mm_setzero_si128()); \
	y_16_2 = _mm_unpackhi_epi8(y, _mm_setzero_si128()); \
	\
	ADD_Y2RGB_16(y_16_1, y_16_2, r_16_1, g_16_1, b_16_1, r_16_2, g_16_2, b_16_2) \
	\
	__m128i r_8_22 = _mm_packus_epi16(r_16_1, r_16_2); \
	__m128i g_8_22 = _mm_packus_epi16(g_16_1, g_16_2); \
	__m128i b_8_22 = _mm_packus_epi16(b_16_1, b_16_2); \
	\
	__m128i rgb_1, rgb_2, rgb_3, rgb_4, rgb_5, rgb_6; \
	\
	PACK_RGB24_32(r_8_11, r_8_12, g_8_11, g_8_12, b_8_11, b_8_12, rgb_1, rgb_2, rgb_3, rgb_4, rgb_5, rgb_6) \
	SAVE_SI128((__m128i*)(rgb_ptr1), rgb_1); \
	SAVE_SI128((__m128i*)(rgb_ptr1+16), rgb_2); \
	SAVE_SI128((__m128i*)(rgb_ptr1+32), rgb_3); \
	SAVE_SI128((__m128i*)(rgb_ptr1+48), rgb_4); \
	SAVE_SI128((__m128i*)(rgb_ptr1+64), rgb_5); \
	SAVE_SI128((__m128i*)(rgb_ptr1+80), rgb_6); \
	\
	PACK_RGB24_32(r_8_21, r_8_22, g_8_21, g_8_22, b_8_21, b_8_22, rgb_1, rgb_2, rgb_3, rgb_4, rgb_5, rgb_6) \
	SAVE_SI128((__m128i*)(rgb_ptr2), rgb_1); \
	SAVE_SI128((__m128i*)(rgb_ptr2+16), rgb_2); \
	SAVE_SI128((__m128i*)(rgb_ptr2+32), rgb_3); \
	SAVE_SI128((__m128i*)(rgb_ptr2+48), rgb_4); \
	SAVE_SI128((__m128i*)(rgb_ptr2+64), rgb_5); \
	SAVE_SI128((__m128i*)(rgb_ptr2+80), rgb_6); \


#define UNPACK_RGB24_32_STEP1(RGB1, RGB2, RGB3, RGB4, RGB5, RGB6, R1, R2, G1, G2, B1, B2) \
R1 = _mm_unpacklo_epi8(RGB1, RGB4); \
R2 = _mm_unpackhi_epi8(RGB1, RGB4); \
G1 = _mm_unpacklo_epi8(RGB2, RGB5); \
G2 = _mm_unpackhi_epi8(RGB2, RGB5); \
B1 = _mm_unpacklo_epi8(RGB3, RGB6); \
B2 = _mm_unpackhi_epi8(RGB3, RGB6);

#define UNPACK_RGB24_32_STEP2(RGB1, RGB2, RGB3, RGB4, RGB5, RGB6, R1, R2, G1, G2, B1, B2) \
RGB1 = _mm_unpacklo_epi8(R1, G2); \
RGB2 = _mm_unpackhi_epi8(R1, G2); \
RGB3 = _mm_unpacklo_epi8(R2, B1); \
RGB4 = _mm_unpackhi_epi8(R2, B1); \
RGB5 = _mm_unpacklo_epi8(G1, B2); \
RGB6 = _mm_unpackhi_epi8(G1, B2); \

#define UNPACK_RGB24_32(RGB1, RGB2, RGB3, RGB4, RGB5, RGB6, R1, R2, G1, G2, B1, B2) \
UNPACK_RGB24_32_STEP1(RGB1, RGB2, RGB3, RGB4, RGB5, RGB6, R1, R2, G1, G2, B1, B2) \
UNPACK_RGB24_32_STEP2(RGB1, RGB2, RGB3, RGB4, RGB5, RGB6, R1, R2, G1, G2, B1, B2) \
UNPACK_RGB24_32_STEP1(RGB1, RGB2, RGB3, RGB4, RGB5, RGB6, R1, R2, G1, G2, B1, B2) \
UNPACK_RGB24_32_STEP2(RGB1, RGB2, RGB3, RGB4, RGB5, RGB6, R1, R2, G1, G2, B1, B2) \
UNPACK_RGB24_32_STEP1(RGB1, RGB2, RGB3, RGB4, RGB5, RGB6, R1, R2, G1, G2, B1, B2) \

#define RGB2YUV_16(R, G, B, Y, U, V) \
Y = _mm_add_epi16(_mm_mullo_epi16(R, _mm_set1_epi16(param->matrix[0][0])), \
		_mm_mullo_epi16(G, _mm_set1_epi16(param->matrix[0][1]))); \
Y = _mm_add_epi16(Y, _mm_mullo_epi16(B, _mm_set1_epi16(param->matrix[0][2]))); \
Y = _mm_add_epi16(Y, _mm_set1_epi16((param->y_shift)<<PRECISION)); \
Y = _mm_srai_epi16(Y, PRECISION); \
U = _mm_add_epi16(_mm_mullo_epi16(R, _mm_set1_epi16(param->matrix[1][0])), \
		_mm_mullo_epi16(G, _mm_set1_epi16(param->matrix[1][1]))); \
U = _mm_add_epi16(U, _mm_mullo_epi16(B, _mm_set1_epi16(param->matrix[1][2]))); \
U = _mm_add_epi16(U, _mm_set1_epi16(128<<PRECISION)); \
U = _mm_srai_epi16(U, PRECISION); \
V = _mm_add_epi16(_mm_mullo_epi16(R, _mm_set1_epi16(param->matrix[2][0])), \
		_mm_mullo_epi16(G, _mm_set1_epi16(param->matrix[2][1]))); \
V = _mm_add_epi16(V, _mm_mullo_epi16(B, _mm_set1_epi16(param->matrix[2][2]))); \
V = _mm_add_epi16(V, _mm_set1_epi16(128<<PRECISION)); \
V = _mm_srai_epi16(V, PRECISION);

#define RGB2YUV_32 \
	__m128i r1, r2, b1, b2, g1, g2; \
	__m128i r_16, g_16, b_16; \
	__m128i y1_16, y2_16, u1_16, u2_16, v1_16, v2_16, y, u1, u2, v1, v2, u1_tmp, u2_tmp, v1_tmp, v2_tmp; \
	__m128i rgb1 = LOAD_SI128((const __m128i*)(rgb_ptr1)), \
		rgb2 = LOAD_SI128((const __m128i*)(rgb_ptr1+16)), \
		rgb3 = LOAD_SI128((const __m128i*)(rgb_ptr1+32)), \
		rgb4 = LOAD_SI128((const __m128i*)(rgb_ptr2)), \
		rgb5 = LOAD_SI128((const __m128i*)(rgb_ptr2+16)), \
		rgb6 = LOAD_SI128((const __m128i*)(rgb_ptr2+32)); \
	/* unpack rgb24 data to r, g and b data in separate channels*/ \
	UNPACK_RGB24_32(rgb1, rgb2, rgb3, rgb4, rgb5, rgb6, r1, r2, g1, g2, b1, b2) \
	/* process pixels of first line */ \
	r_16 = _mm_unpacklo_epi8(r1, _mm_setzero_si128()); \
	g_16 = _mm_unpacklo_epi8(g1, _mm_setzero_si128()); \
	b_16 = _mm_unpacklo_epi8(b1, _mm_setzero_si128()); \
	RGB2YUV_16(r_16, g_16, b_16, y1_16, u1_16, v1_16) \
	r_16 = _mm_unpackhi_epi8(r1, _mm_setzero_si128()); \
	g_16 = _mm_unpackhi_epi8(g1, _mm_setzero_si128()); \
	b_16 = _mm_unpackhi_epi8(b1, _mm_setzero_si128()); \
	RGB2YUV_16(r_16, g_16, b_16, y2_16, u2_16, v2_16) \
	y = _mm_packus_epi16(y1_16, y2_16); \
	u1 = _mm_packus_epi16(u1_16, u2_16); \
	v1 = _mm_packus_epi16(v1_16, v2_16); \
	/* save Y values */ \
	SAVE_SI128((__m128i*)(y_ptr1), y); \
	/* process pixels of second line */ \
	r_16 = _mm_unpacklo_epi8(r2, _mm_setzero_si128()); \
	g_16 = _mm_unpacklo_epi8(g2, _mm_setzero_si128()); \
	b_16 = _mm_unpacklo_epi8(b2, _mm_setzero_si128()); \
	RGB2YUV_16(r_16, g_16, b_16, y1_16, u1_16, v1_16) \
	r_16 = _mm_unpackhi_epi8(r2, _mm_setzero_si128()); \
	g_16 = _mm_unpackhi_epi8(g2, _mm_setzero_si128()); \
	b_16 = _mm_unpackhi_epi8(b2, _mm_setzero_si128()); \
	RGB2YUV_16(r_16, g_16, b_16, y2_16, u2_16, v2_16) \
	y = _mm_packus_epi16(y1_16, y2_16); \
	u2 = _mm_packus_epi16(u1_16, u2_16); \
	v2 = _mm_packus_epi16(v1_16, v2_16); \
	/* save Y values */ \
	SAVE_SI128((__m128i*)(y_ptr2), y); \
	/* vertical subsampling of u/v values */ \
	u1_tmp = _mm_avg_epu8(u1, u2); \
	v1_tmp = _mm_avg_epu8(v1, v2); \
	/* do the same again with next data */ \
	rgb1 = LOAD_SI128((const __m128i*)(rgb_ptr1+48)); \
	rgb2 = LOAD_SI128((const __m128i*)(rgb_ptr1+64)); \
	rgb3 = LOAD_SI128((const __m128i*)(rgb_ptr1+80)); \
	rgb4 = LOAD_SI128((const __m128i*)(rgb_ptr2+48)); \
	rgb5 = LOAD_SI128((const __m128i*)(rgb_ptr2+64)); \
	rgb6 = LOAD_SI128((const __m128i*)(rgb_ptr2+80)); \
	/* unpack rgb24 data to r, g and b data in separate channels*/ \
	UNPACK_RGB24_32(rgb1, rgb2, rgb3, rgb4, rgb5, rgb6, r1, r2, g1, g2, b1, b2) \
	/* process pixels of first line */ \
	r_16 = _mm_unpacklo_epi8(r1, _mm_setzero_si128()); \
	g_16 = _mm_unpacklo_epi8(g1, _mm_setzero_si128()); \
	b_16 = _mm_unpacklo_epi8(b1, _mm_setzero_si128()); \
	RGB2YUV_16(r_16, g_16, b_16, y1_16, u1_16, v1_16) \
	r_16 = _mm_unpackhi_epi8(r1, _mm_setzero_si128()); \
	g_16 = _mm_unpackhi_epi8(g1, _mm_setzero_si128()); \
	b_16 = _mm_unpackhi_epi8(b1, _mm_setzero_si128()); \
	RGB2YUV_16(r_16, g_16, b_16, y2_16, u2_16, v2_16) \
	y = _mm_packus_epi16(y1_16, y2_16); \
	u1 = _mm_packus_epi16(u1_16, u2_16); \
	v1 = _mm_packus_epi16(v1_16, v2_16); \
	/* save Y values */ \
	SAVE_SI128((__m128i*)(y_ptr1+16), y); \
	/* process pixels of second line */ \
	r_16 = _mm_unpacklo_epi8(r2, _mm_setzero_si128()); \
	g_16 = _mm_unpacklo_epi8(g2, _mm_setzero_si128()); \
	b_16 = _mm_unpacklo_epi8(b2, _mm_setzero_si128()); \
	RGB2YUV_16(r_16, g_16, b_16, y1_16, u1_16, v1_16) \
	r_16 = _mm_unpackhi_epi8(r2, _mm_setzero_si128()); \
	g_16 = _mm_unpackhi_epi8(g2, _mm_setzero_si128()); \
	b_16 = _mm_unpackhi_epi8(b2, _mm_setzero_si128()); \
	RGB2YUV_16(r_16, g_16, b_16, y2_16, u2_16, v2_16) \
	y = _mm_packus_epi16(y1_16, y2_16); \
	u2 = _mm_packus_epi16(u1_16, u2_16); \
	v2 = _mm_packus_epi16(v1_16, v2_16); \
	/* save Y values */ \
	SAVE_SI128((__m128i*)(y_ptr2+16), y); \
	/* vertical subsampling of u/v values */ \
	u2_tmp = _mm_avg_epu8(u1, u2); \
	v2_tmp = _mm_avg_epu8(v1, v2); \
	/* horizontal subsampling of u/v values */ \
	u1 = _mm_packus_epi16(_mm_srl_epi16(u1_tmp, _mm_cvtsi32_si128(8)), _mm_srl_epi16(u2_tmp, _mm_cvtsi32_si128(8))); \
	v1 = _mm_packus_epi16(_mm_srl_epi16(v1_tmp, _mm_cvtsi32_si128(8)), _mm_srl_epi16(v2_tmp, _mm_cvtsi32_si128(8))); \
	u2 = _mm_packus_epi16(_mm_and_si128(u1_tmp, _mm_set1_epi16(0xFF)), _mm_and_si128(u2_tmp, _mm_set1_epi16(0xFF))); \
	v2 = _mm_packus_epi16(_mm_and_si128(v1_tmp, _mm_set1_epi16(0xFF)), _mm_and_si128(v2_tmp, _mm_set1_epi16(0xFF))); \
	u1 = _mm_avg_epu8(u1, u2); \
	v1 = _mm_avg_epu8(v1, v2); \
	SAVE_SI128((__m128i*)(u_ptr), u1); \
	SAVE_SI128((__m128i*)(v_ptr), v1);

void yuv420_rgb24_sse(
	uint32_t width, uint32_t height, 
	const uint8_t *Y, const uint8_t *U, const uint8_t *V, uint32_t Y_stride, uint32_t UV_stride, 
	uint8_t *RGB, uint32_t RGB_stride, 
	YCbCrType yuv_type)
{
	#define LOAD_SI128 _mm_load_si128
	#define SAVE_SI128 _mm_stream_si128
	const YUV2RGBParam *const param = &(YUV2RGB[yuv_type]);
	
	uint32_t x, y;
	for(y=0; y<(height-1); y+=2)
	{
		const uint8_t *y_ptr1=Y+y*Y_stride,
			*y_ptr2=Y+(y+1)*Y_stride,
			*u_ptr=U+(y/2)*UV_stride,
			*v_ptr=V+(y/2)*UV_stride;
		
		uint8_t *rgb_ptr1=RGB+y*RGB_stride,
			*rgb_ptr2=RGB+(y+1)*RGB_stride;
		
		for(x=0; x<(width-31); x+=32)
		{
			YUV2RGB_32
			
			y_ptr1+=32;
			y_ptr2+=32;
			u_ptr+=16; 
			v_ptr+=16;
			rgb_ptr1+=96;
			rgb_ptr2+=96;
		}
	}
	#undef LOAD_SI128
	#undef SAVE_SI128
}

void yuv420_rgb24_sseu(
	uint32_t width, uint32_t height, 
	const uint8_t *Y, const uint8_t *U, const uint8_t *V, uint32_t Y_stride, uint32_t UV_stride, 
	uint8_t *RGB, uint32_t RGB_stride, 
	YCbCrType yuv_type)
{
	#define LOAD_SI128 _mm_loadu_si128
	#define SAVE_SI128 _mm_storeu_si128
	const YUV2RGBParam *const param = &(YUV2RGB[yuv_type]);
	
	uint32_t x, y;
	for(y=0; y<(height-1); y+=2)
	{
		const uint8_t *y_ptr1=Y+y*Y_stride,
			*y_ptr2=Y+(y+1)*Y_stride,
			*u_ptr=U+(y/2)*UV_stride,
			*v_ptr=V+(y/2)*UV_stride;
		
		uint8_t *rgb_ptr1=RGB+y*RGB_stride,
			*rgb_ptr2=RGB+(y+1)*RGB_stride;
		
		for(x=0; x<(width-31); x+=32)
		{
			YUV2RGB_32
			
			y_ptr1+=32;
			y_ptr2+=32;
			u_ptr+=16; 
			v_ptr+=16;
			rgb_ptr1+=96;
			rgb_ptr2+=96;
		}
	}
	#undef LOAD_SI128
	#undef SAVE_SI128
}

void rgb24_yuv420_sse(uint32_t width, uint32_t height, 
	const uint8_t *RGB, uint32_t RGB_stride, 
	uint8_t *Y, uint8_t *U, uint8_t *V, uint32_t Y_stride, uint32_t UV_stride, 
	YCbCrType yuv_type)
{
	#define LOAD_SI128 _mm_load_si128
	#define SAVE_SI128 _mm_stream_si128
	const RGB2YUVParam *const param = &(RGB2YUV[yuv_type]);
	
	uint32_t x, y;
	for(y=0; y<(height-1); y+=2)
	{
		const uint8_t *rgb_ptr1=RGB+y*RGB_stride,
			*rgb_ptr2=RGB+(y+1)*RGB_stride;
		
		uint8_t *y_ptr1=Y+y*Y_stride,
			*y_ptr2=Y+(y+1)*Y_stride,
			*u_ptr=U+(y/2)*UV_stride,
			*v_ptr=V+(y/2)*UV_stride;
		
		for(x=0; x<(width-31); x+=32)
		{
			RGB2YUV_32
			
			rgb_ptr1+=96;
			rgb_ptr2+=96;
			y_ptr1+=32;
			y_ptr2+=32;
			u_ptr+=16; 
			v_ptr+=16;
		}
	}
	#undef LOAD_SI128
	#undef SAVE_SI128
}

void rgb24_yuv420_sseu(uint32_t width, uint32_t height, 
	const uint8_t *RGB, uint32_t RGB_stride, 
	uint8_t *Y, uint8_t *U, uint8_t *V, uint32_t Y_stride, uint32_t UV_stride, 
	YCbCrType yuv_type)
{
	#define LOAD_SI128 _mm_loadu_si128
	#define SAVE_SI128 _mm_storeu_si128
	const RGB2YUVParam *const param = &(RGB2YUV[yuv_type]);
	
	uint32_t x, y;
	for(y=0; y<(height-1); y+=2)
	{
		const uint8_t *rgb_ptr1=RGB+y*RGB_stride,
			*rgb_ptr2=RGB+(y+1)*RGB_stride;
		
		uint8_t *y_ptr1=Y+y*Y_stride,
			*y_ptr2=Y+(y+1)*Y_stride,
			*u_ptr=U+(y/2)*UV_stride,
			*v_ptr=V+(y/2)*UV_stride;
		
		for(x=0; x<(width-31); x+=32)
		{
			RGB2YUV_32
			
			rgb_ptr1+=96;
			rgb_ptr2+=96;
			y_ptr1+=32;
			y_ptr2+=32;
			u_ptr+=16; 
			v_ptr+=16;
		}
	}
	#undef LOAD_SI128
	#undef SAVE_SI128
}


#endif //__SSE2__
#endif

//alpha = desc -> alpha
//filterSize = instance->filter_size
//filterPos = instance->filter_pos	(read only, arr)
//filter = instance -> filter		(read only, arr)
//dstW = dstW

//sliceH = sliceH
//sliceY = sliceY

//uint8_t* src = desc->src->plane[0].line	(read only, uint8_t arr [sizeof(uint8_t) * dstW * sliceH])		array size unit is BYTE
//uint16_t* dst = desc->dst->plane[0].line	(write,		uint8_t arr [sizeof(uint16_t) * dstW * sliceH])
//uint8_t* src3 = desc->src->plane[3].line	(read only, uint8_t arr [sizeof(uint8_t) * dstW * sliceH])
//uint16_t* dst3 = desc->dst->plane[3].line	(write,		uint8_t arr [sizeof(uint16_t) * dstW * sliceH])

//src0_sliceY = desc->src->plane[0].sliceY			(read only, int)
//dst0_sliceY = desc->dst->plane[0].sliceY			(read only, int)

//src3_sliceY = desc->src->plane[3].sliceY			(read only, int)
//dst3_sliceY = desc->dst->plane[3].sliceY			(read only, int)

//convertOn = c->lumConvertRange != NULL			(read only, int)

//sliceH++ should be at host

__kernel void hScale8To15_c(int dstW, int alpha,
			__global short* filter,
			__global int* filterPos, int filterSize,
			__global int* sliceHp, __global int* sliceYp, 
			__global ushort* dst, __global uchar* src,
			__global ushort* dst3, __global uchar* src3,
			int convertOn,
			int src0_sliceY, __global int* dst0_sliceYp,
			int src3_sliceY, __global int* dst3_sliceYp, int cl_srcW, int cl_dstW,\
                         int  available_lines)

{

    int sliceH = *sliceHp;
    int sliceY = *sliceYp;
    int dst0_sliceY = *dst0_sliceYp;
    int dst3_sliceY = *dst3_sliceYp;
    int i = get_global_id(0);
    if(i < sliceH){

		int src_pos = (sliceY + i - src0_sliceY) * cl_srcW;
		int dst_pos = (sliceY + i - dst0_sliceY);
                while(dst_pos >= available_lines)
                    dst_pos -= available_lines;

                dst_pos = dst_pos * cl_dstW;

		int j;
		for (j = 0; j < dstW; j++) {
			int k;
			int srcPos = filterPos[j];
			int val    = 0;
			for (k = 0; k < filterSize; k++) {
				val += ((int)src[src_pos + srcPos + k]) * filter[filterSize * j + k];
				//val += ((int)src[src_pos + srcPos + k]);
				//val += filter[filterSize * j + k];
			}
			if ((val >> 7) > (1 << 15) - 1) {
                            dst[dst_pos + j] = (1 << 15) - 1;
			} else {
                            dst[dst_pos + j] = (val >> 7);
                        }
		}

                //tmp
		//if(convertOn){
		//	int l;
		//	for(l=0; l<dstW; l++){
		//		dst[dst_pos+l] = dst[dst_pos+l] * 14071 + 33561947 >> 14;
		//	}
		//}
		//desc->dst->plane[0].sliceH += 1;


        if (alpha) {
	
		int src_pos = (sliceY + i - src3_sliceY) * cl_srcW;
		int dst_pos = (sliceY + i - dst3_sliceY) * cl_dstW;

	    //desc->dst->plane[3].sliceH += 1;
	
	    int j;
            for (j = 0; j < dstW; j++) {
	
                int k;
                int srcPos = filterPos[j];
                int val    = 0;
                for (k = 0; k < filterSize; k++) {
                    val += ((int)src[srcPos + k]) * filter[filterSize * j + k];
                }
	
		if ((val >> 7) >(1 << 15) - 1)
			dst[dst_pos + j] = (1 << 15) - 1;
		else
			dst[dst_pos + j] = (val >> 7);
	
            }
	
        }   //if(desc->alpha) end
	
    }

}

/*
    int i;
    for (i = 0; i < dstW; i++) {
        int j;
        int srcPos = filterPos[i];
        int val    = 0;
        for (j = 0; j < filterSize; j++) {
            val += ((int)src[srcPos + j]) * filter[filterSize * i + j];
        }
        dst[i] = FFMIN(val >> 7, (1 << 15) - 1); // the cubic equation does overflow ...
    }
*/


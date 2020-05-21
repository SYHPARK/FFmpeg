__kernel void Thumbnail_uchar(const int offset, const int W, const int H, __global int* hist, __global char* src){
	int w = get_global_id(0);
	int h;
	if (w < W){
		for(h=0; h<H; h++){
			atomic_add(&hist[offset + (src[(w*H+h)+0]+128)], 1);
		}
	}
}

__kernel void Thumbnail_uchar2(const int offset, const int W, const int H, __global int* hist, __global char* src){
	int w = get_global_id(0);
	int h;
	if (w < W){
		for(h=0; h<H; h++){
			atomic_add(&hist[offset + (src[(w*H+h)+0]+128)], 1);
			atomic_add(&hist[offset + 256 + (src[(w*H+h)+1]+128)], 1);
		}
	}
}

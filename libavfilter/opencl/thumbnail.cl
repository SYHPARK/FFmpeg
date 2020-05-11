//__kernel void Thumbnail_uchar(__global char* src, const int offset, const int W, const int H, __global int* hist){
__kernel void Thumbnail_uchar(const int offset, const int W, const int H, __global int* hist, __global char* src){
	int w = get_global_id(0);
	int h = get_global_id(1);
	if (w < W && h < H){
		atomic_add(&hist[offset + src[(w*H+h)*3+0]], 1);
	}
}

//__kernel void Thumbnail_uchar2(__global char* src, const int offset, const int W, const int H, __global int* hist){
__kernel void Thumbnail_uchar2(const int offset, const int W, const int H, __global int* hist, __global char* src){
	int w = get_global_id(0);
	int h = get_global_id(1);
	if (w < W && h < H){
		atomic_add(&hist[offset + src[(w*H+h)*3+0]], 1);
		atomic_add(&hist[offset + 256 + src[(w*H+h)*3+1]], 1);
	}
}

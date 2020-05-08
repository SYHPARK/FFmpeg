__kernel void InvertImage(__global char* src, __global char* dst, const int X, const int Y, __global int* hist){
	int x = get_global_id(0);
	int y = get_global_id(1);
	if (x < X && y < Y){
	    	dst[(x*Y+y)*3+0] = src[(x*Y+y)*3+0];
		atomic_add(&hist[0*256 + src[(x*Y+y)*3+0]], 1);
		dst[(x*Y+y)*3+1] = src[(x*Y+y)*3+1];
		atomic_add(&hist[1*256+ src[(x*Y+y)*3+1]], 1);
		dst[(x*Y+y)*3+2] = src[(x*Y+y)*3+2];
		atomic_add(&hist[2*256+ src[(x*Y+y)*3+2]], 1);
	}
}

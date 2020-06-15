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

__kernel void _is_good_pgm (__global uchar *buf, const int wrap, const int xsize, const int ysize, __global int* point, __global int* sum_diff)
{
    int target_y_offset = get_global_id(0);

    int i;
    int step;
    __global char *cnt;     
    int is_different;
    int cnt_offset;
    int tmp;

    /*set center line*/
    step = ysize >> 3;
    cnt_offset = ( ysize >> 1 );
    cnt = buf + cnt_offset * wrap;


    if (target_y_offset != cnt_offset)
    {
        if(target_y_offset % step == 0) {
            __global char *s = cnt;
            __global char* d = buf + target_y_offset * wrap;
            is_different = 0;
            for (int i = 0; i < xsize; i++) {
                     if (*s++ != *d++) {
                         tmp = (*s - *d);
                         is_different += (tmp < 0 ? -tmp : tmp);
                         //tmp
                         is_different = 0xaa;
                     }
            }
            is_different /= xsize;

            point[target_y_offset] = (is_different != 0);
            sum_diff[target_y_offset] = is_different;
        } else {
            point[target_y_offset] = -1;
            sum_diff[target_y_offset] = -1;
        }
    } else {
        point[target_y_offset] = -1;
        sum_diff[target_y_offset] = -1;
    }
}


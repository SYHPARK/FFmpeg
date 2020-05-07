#gpu ocl:0.0, cpu ocl: 1.0

time LD_PRELOAD=/home/odroid/workingDirectory/gpgpu/lib/libclsched.so ffmpeg -init_hw_device opencl=ocl:1.0 \
-filter_hw_device ocl \
-i 4kvideo.mp4 \
-vf "format=nv12,hwupload,thumbnail_opencl,hwdownload,format=nv12" \
-y ~/4kthumbnail.mp4 

#cpu
#time ffmpeg -i bunny.mp4 -vf thumbnail -y thumbnail_cpu.mp4

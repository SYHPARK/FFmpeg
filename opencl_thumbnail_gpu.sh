#gpu ocl:0.0, cpu ocl: 1.0

time ./ffmpeg -init_hw_device opencl=ocl:0.0 \
-filter_hw_device ocl \
-report \
-i bunny.mp4 \
-vf "format=nv12,hwupload,thumbnail_opencl=2,hwdownload,format=nv12" \
-vframes 1 \
-y thumbnail.jpg

#cpu
#time ffmpeg -i bunny.mp4 -vf thumbnail -y thumbnail_cpu.mp4

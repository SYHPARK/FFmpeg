#gpu ocl:0.0, cpu ocl: 1.0

./ffmpeg -init_hw_device opencl=ocl:3.0 \
-filter_hw_device ocl \
-i bunny_30s.mp4 \
-vf "format=nv12,hwupload,thumbnail_opencl=2,hwdownload,format=nv12" \
-vframes 1 \
-y ./thumbnail_bunny.jpg

#cpu
#time ffmpeg -i bunny.mp4 -vf thumbnail -y thumbnail_cpu.mp4

#cpu
#time \
#LD_PRELOAD=../../gpgpu/lib/libclsched.so \
#ffmpeg -loglevel debug -an -init_hw_device opencl=ocl:1.0 \
#                -filter_hw_device ocl -i bunny.mp4 -vf \
#                "format=nv12,hwupload,thumbnail_opencl,hwdownload,format=nv12" \
#                -vframes 1 example/thumb.jpg

#gpu
time \
LD_PRELOAD=../../gpgpu/lib/libclsched.so \
ffmpeg -loglevel debug -an -init_hw_device opencl=ocl:0.0 \
	-filter_hw_device ocl -i bunny.mp4 -vf \
	"format=nv12,hwupload,thumbnail_opencl,hwdownload,format=nv12" \
	-y -vframes 1 example/thumb.jpg

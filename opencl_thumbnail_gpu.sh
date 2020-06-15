#gpu ocl:0.0, cpu ocl: 1.0

if [ "$2" == "hybridcl" ]; then
    echo "seralee !!!!!!!! hybridcl version"

LD_PRELOAD="./libavdevice/libavdevice.so libavfilter/libavfilter.so.7 ./libavformat/libavformat.so ./libavcodec/libavcodec.so.58 ./libpostproc/libpostproc.so.55 ./libswresample/libswresample.so.3 ./libswscale/libswscale.so.5 ./libavutil/libavutil.so.56 /home/seralee/SampleCodes/gpgpu_scaling_bugfix/gpgpu/lib/libclsched.so" ./ffmpeg -init_hw_device opencl=ocl:0.0 \
-filter_hw_device ocl \
-report \
-i bunny.mp4 \
-vf "format=nv12,hwupload,thumbnail_opencl=2,hwdownload,format=nv12,scale" \
-vframes 1 \
-y thumbnail.jpg
elif [ "$2" == "hybridcl2" ]; then
    

LD_PRELOAD="./libavdevice/libavdevice.so libavfilter/libavfilter.so.7 ./libavformat/libavformat.so ./libavcodec/libavcodec.so.58 ./libpostproc/libpostproc.so.55 ./libswresample/libswresample.so.3 ./libswscale/libswscale.so.5 ./libavutil/libavutil.so.56 /home/seralee/HybridCL/libHybridCL.so" ./ffmpeg -init_hw_device opencl=ocl:0.0 \
-filter_hw_device ocl \
-report \
-i bunny.mp4 \
-vf "format=nv12,hwupload,thumbnail_opencl=2,hwdownload,format=nv12,scale" \
-vframes 1 \
-y thumbnail.jpg

else

LD_PRELOAD="./libavdevice/libavdevice.so libavfilter/libavfilter.so.7 ./libavformat/libavformat.so ./libavcodec/libavcodec.so.58 ./libpostproc/libpostproc.so.55 ./libswresample/libswresample.so.3 ./libswscale/libswscale.so.5 ./libavutil/libavutil.so.56" ./ffmpeg -init_hw_device opencl=ocl:0.0 \
-filter_hw_device ocl \
-report \
-i bunny.mp4 \
-vf "format=nv12,hwupload,thumbnail_opencl=2,hwdownload,format=nv12,scale" \
-vframes 1 \
-y thumbnail.jpg
fi

#cpu
#time ffmpeg -i bunny.mp4 -vf thumbnail -y thumbnail_cpu.mp4

FFmpeg README
=============

FFmpeg is a collection of libraries and tools to process multimedia content
such as audio, video, subtitles and related metadata.

## Libraries

* `libavcodec` provides implementation of a wider range of codecs.
* `libavformat` implements streaming protocols, container formats and basic I/O access.
* `libavutil` includes hashers, decompressors and miscellaneous utility functions.
* `libavfilter` provides a mean to alter decoded Audio and Video through chain of filters.
* `libavdevice` provides an abstraction to access capture and playback devices.
* `libswresample` implements audio mixing and resampling routines.
* `libswscale` implements color conversion and scaling routines.

## Tools

* [ffmpeg](https://ffmpeg.org/ffmpeg.html) is a command line toolbox to
  manipulate, convert and stream multimedia content.
* [ffplay](https://ffmpeg.org/ffplay.html) is a minimalistic multimedia player.
* [ffprobe](https://ffmpeg.org/ffprobe.html) is a simple analysis tool to inspect
  multimedia content.
* Additional small tools such as `aviocat`, `ismindex` and `qt-faststart`.

## Documentation

The offline documentation is available in the **doc/** directory.

The online documentation is available in the main [website](https://ffmpeg.org)
and in the [wiki](https://trac.ffmpeg.org).

### Examples

Coding examples are available in the **doc/examples** directory.

## License

FFmpeg codebase is mainly LGPL-licensed with optional components licensed under
GPL. Please refer to the LICENSE file for detailed information.

## Contributing

Patches should be submitted to the ffmpeg-devel mailing list using
`git format-patch` or `git send-email`. Github pull requests should be
avoided because they are not part of our review process and will be ignored.

## OpenCL Thumbnail

* Currently supported formats: NV12, YUV420P, and YUV444P

* Using Platform 0 (CPU)
```console
$ time ffmpeg -init_hw_device opencl=ocl:0.0 -filter_hw_device ocl -i sample.mp4 -vf "format=nv12,hwupload,thumbnail_opencl,hwdownload,format=nv12" -y thumbnail.mp4

5.41s user 0.23s system 449% cpu 1.254 total
```

* Using Platform 1 (GPU)
```console
$ time ffmpeg -init_hw_device opencl=ocl:1.0 -filter_hw_device ocl -i sample.mp4 -vf "format=nv12,hwupload,thumbnail_opencl,hwdownload,format=nv12" -y thumbnail.mp4

1.53s user 0.21s system 178% cpu 0.972 total
```

* TODO
		- Supporting P010, P016, and YUV444P16

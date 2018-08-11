# LOCAL_PATH := $(call my-dir)
# include $(CLEAR_VARS)
# LOCAL_MODULE    := ffmpeg
# LOCAL_SRC_FILES := fftools/ffmpeg.c fftools/ffmpeg_opt.c fftools/cmdutils.c fftools/ffmpeg_filter.c fftools/ffmpeg_hw.c
# LOCAL_LDLIBS := -llog
# LOCAL_SHARED_LIBRARIES := libavformat libavcodec libswscale libavutil libswresample libavfilter
# include $(BUILD_SHARED_LIBRARY)
# $(call import-module, ffmpeg/android/arm)

LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES := custom_ffplay.c
LOCAL_LDLIBS += -llog -lz -landroid
LOCAL_MODULE := VideoPlayer
LOCAL_C_INCLUDES += $(LOCAL_PATH)/include

LOCAL_SHARED_LIBRARIES:= avcodec avformat avutil swresample swscale

include $(BUILD_SHARED_LIBRARY)
$(call import-module, ffmpeg/android/arm)
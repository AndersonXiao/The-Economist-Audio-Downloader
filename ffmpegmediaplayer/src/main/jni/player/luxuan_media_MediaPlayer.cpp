//
// Created by Administrator on 2019/5/16.
//
#define LOG_TAG "FFmpegMediaPlayer-JNI"

#include "android/log.h"
#include <mediaplayer.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include "jni.h"
#include "Errors.h"

#include <android/bitmap.h>
#include <android/native_window_jni.h>

extern "C"
{
#include "ffmpeg_mediaplayer.h"
}

using namespace std;

struct fields_t
{
    jfieldID context;
    jfieldID surface_texture;

    jmethodID post_event;
};

static fields_t fields;

static JavaVM *m_vm;

class JNIMediaPlayerListener: public MediaPlayerListener
{
public:
    JNIMediaPlayerListener(JNIEnv *env, jobject thiz, jobject weak_thiz);
    ~JNIMediaPlayerListener();
    virtual void notify(int msg, int ext1, int ext2, int from_thread);

private:
    JNIMediaPlayerListener();
    jclass mClass;
    jobject mObject;
    jobject mThiz;
};

void jniThrowException(JNIEnv *env, const char *className, const char *msg)
{
    jclass exception = env->FindClass(className);
    env->ThrowNew(exception, msg);
}

JNIMediaPlayerListener::JNIMediaPlayerListener(JNIEnv* env, jobject thiz, jobject weak_thiz)
{
    jclass clazz=env->GetObjectClass(thiz);
    if(clazz==NULL)
    {
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "Can't find luxuan/media/FFmpegMediaPlayer");
        jniThrowException(env, "java/lang/Exception", NULL);
        return;
    }
    mClass=(jclass)env->NewGlobalRef(clazz);
    mThiz=(jobject)env->NewGlobalRef(thiz);
    mObject=env->NewGlobalRef(weak_thiz);
}

void JNIMediaPlayerListener::notify(int msg, int ext1, int ext2, int fromThread)
{
    __android_log_print(ANDROID_LOG_VERBOSE, LOG_TAG, "notify: %d", msg);
    JNIEnv *env=0;
    int isAttached=0;

    int status=m_vm->GetEnv((void **)&env, JNI_VERSION_1_6);

    if(fromThread)
    {
        jclass *interface_class;

        isAttached=0;

        if(m_vm->AttachCurrentThread(&env, NULL)<0)
        {
            __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "failed to attach current thread");
        }

        isAttached=1;
    }

    env->CallStaticVoidMethod(mClass, fields.post_event, mObject, msg, ext1, ext2, NULL);

    if(env->ExceptionCheck())
    {
        __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "An exception occurred while notifying an event.");
        env->ExceptionClear();
    }

    if(fromThread&&isAttached)
    {
        m_vm->DetachCurrentThread();
    }
}

static MediaPlayer* getMediaPlayer(JNIEnv *env, jobject thiz)
{
    MediaPlayer *const p=(MediaPlayer*)env->GetLongField(thiz, fields.context);
    return p;
}

static MediaPlayer* setMediaPlayer(JNIEnv* env, jobject thiz, long mp)
{
    MediaPlayer* old=(MediaPlayer*)env->GetLongField(thiz, fields.context);
    env->SetLongField(thiz, fields.context, mp);
    return old;
}

static void process_media_player_call(JNIEnv *env, jobject thiz, int opStatus, const char* exception, const char* message)
{
    if(exception==NULL)
    {
        if(opStatus!=(int)OK)
        {
            MediaPlayer* mp=getMediaPlayer(env, thiz);
            if(mp!=0)
                mp->notify(MEDIA_ERROR, opStatus, 0, 0);
        }
    }
    else
    {
        if(opStatus==(int)INVALID_OPERATION)
        {
            jniThrowException(env, "java/lang/IllegalStateException", NULL);
        }
        else if(opStatus==(int) PERMISSION_DENIED)
        {
            jniThrowException(env, "java/lang/SecurityException", NULL);
        }
        else if(opStatus!=(int)OK)
        {
            if(strlen(message)>230)
            {
                jniThrowException(env, exception, message);
            }
            else
            {
                char msg[256];
                sprintf(msg, "%s: status=0x%x", message, opStatus);
                jniThrowException(env, exception, msg);
            }
        }
    }
}

static void luxuan_media_FFmpegMediaPlayer_setDataSourceAndHeaders(JNIEnv *env, jobject thiz, jstring path, jobjectArray keys, jobjectArray values)
{
    MediaPlayer *mp=getMediaPlayer(env, thiz);
    if(mp==NULL)
    {
        jniThrowException(env, "java/lang/IllegalStateException", NULL);
        return;
    }

    if(path==NULL)
    {
        jniThrowException(env, "java/lang/IllegalArgumentException", NULL);
        return;
    }

    const char *tmp=env->GetStringUTFChars(path, NULL);
    if(tmp==NULL)
    {
        return;
    }

    char *restrict_to=(char *)strstr(tmp, "mms://");
    if(restrict_to)
    {
        strncpy(restrict_to, "mmsh://", 6);
        puts(tmp);
    }

    char *headers=NULL;

    if(keys&&values!=NULL)
    {
        int keysCount=env->GetArrayLength(keys);
        int valuesCount=env->GetArrayLength(values);

        if(keysCount!=valuesCount)
        {
            __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "keys and values arrays have differnt length");
            jniThrowException(env, "java/lang/IllegalArgumentException", NULL);
            return;
        }

        int i=0;
        const char *rawString=NULL;
        char hdrs[2048];

        strcpy(hdrs, "");

        for(i=0;i<keysCount;i++)
        {
            jstring key=(jstring)env->GetObjectArrayElement(keys, i);
            rawString=env->GetStringUTFChars(key, NULL);
            strcat(hdrs, rawString);
            strcat(hdrs, ": ");
            env->ReleaseStringUTFChars(key, rawString);

            jstring value=(jstring) env->GetObjectArrayElement(values, i);
            rawString=env->GetStringUTFChars(value, NULL);
            strcat(hdrs, rawString);
            strcat(hdrs, "\r\n");
            env->ReleaseStringUTFChars(value, rawString);
        }

        headers=&hdrs[0];
    }

    __android_log_print(ANDROID_LOG_VERBOSE, LOG_TAG, "setDataSource: path %s", tmp);

    status_t opStatus=mp->setDataSource(tmp, headers);

    process_media_player_call(env, thiz, opStatus, "java/io/IOException", "setDataSource failed.");

    env->ReleaseStringUTFChars(path, tmp);
    tmp=NULL;
}

static int jniGetFDFromFileDescriptor(JNIEnv *env, jobject fileDescriptor)
{
    jint fd=-1;
    jclass fdClass=env->FindClass("java/io/FileDescriptor");

    if(fdClass!=NULL)
    {
        jfieldID fdClassDescriptorFieldID=env->GetFieldID(fdClass, "descriptor", "I");
        if(fdClassDescriptorFieldID!=NULL && fileDescriptor!=NULL)
        {
            fd=env->GetIntField(fileDescriptor, fdClassDescriptorFieldID);
        }
    }

    return fd;
}

static void luxuan_media_FFmpegMediaPlayer_setDataSourceFD(JNIEnv *env, jobject thiz, jobject fileDescriptor, jlong offset, jlong length)
{
    MediaPlayer *mp=getMediaPlayer(env, thiz);
    if(mp==NULL)
    {
        jniThrowException(env, "java/lang/IllegalStateException", NULL);
        return;
    }

    if(fileDescriptor==NULL)
    {
        jniThrowException(env, "java/lang/IllegalArgumentException", NULL);
        return;
    }

    int fd=jniGetFDFromFileDescriptor(env, fileDescriptor);
    __android_log_print(ANDROID_LOG_VERBOSE, LOG_TAG, "setDataSourceFD: fd %d", fd);
    process_media_player_call(env, thiz, mp->setDataSource(fd, offset, length), "java/io/IOException", "setDataSourceFD failed.");
}

static void decVideoSurfaceRef(JNIEnv *env, jobject thiz)
{

}

static void setVideoSurface(JNIEnv *env, jobject thiz, jobject jsurface, jboolean mediaPlayerMustBeAlive)
{
    MediaPlayer *mp=getMediaPlayer(env, thiz);
    if(mp==NULL)
    {
        if(mediaPlayerMustBeAlive)
        {
            jniThrowException(env, "java.lang/IllegalStateException", NULL);
        }

        return;
    }

    decVideoSurfaceRef(env, thiz);

    ANativeWindow* theNativeWindow=ANativeWindow_fromSurface(env, jsurface);

    if(theNativeWindow!=NULL)
    {
        mp->setVideoSurface(theNativeWindow);
    }
}

static void luxuan_media_FFmpegMediaPlayer_setVideoSurface(JNIEnv *env, jobject thiz, jobject jsurface)
{
    setVideoSurface(env, thiz, jsurface, true);
}

static void luxuan_media_FFmpegMediaPlayer_prepare(JNIEnv *env, jobject thiz)
{
    MediaPlayer* mp=getMediaPlayer(env, thiz);

    if(mp==NULL)
    {
        jniThrowException(env, "java/lang/IllegalStateException", NULL);
        return;
    }

    process_media_player_call(env, thiz, mp->prepare(), "java/io/IOException", "Prepare failed.");
}

static void luxuan_meida_FFmpegMediaPlayer_prepareAsync(JNIEnv *env, jobject thiz)
{
    MediaPlayer* mp=getMediaPlayer(env, thiz);
    if(mp==NULL)
    {
        jniThrowException(env, "java/lang/IllegalStateException", NULL);
        return;
    }

    process_media_player_call(env, thiz, mp->prepareAsync(), "java/io/IOException", "Prepare Async failed.");
}

static void luxuan_media_FFmpegMediaPlayer_start(JNIEnv *env, jobject thiz)
{
    __android_log_print(ANDROID_LOG_VERBOSE, LOG_TAG, "start");
    MediaPlayer *mp=getMediaPlayer(env, thiz);
    if(mp==NULL)
    {
        jniThrowException(env, "java/lang/IllegalStateException",NULL);
        return;
    }

    process_media_player_call(env, thiz, mp->start(), NULL, NULL);
}

static void luxuan_media_FFmpegMediaPlayer_stop(JNIEnv *env, jobject thiz)
{
    __android_log_print(ANDROID_LOG_VERBOSE, LOG_TAG, "stop");
    MediaPlayer* mp=getMediaPlayer(env, thiz);
    if(mp==NULL)
    {
        jniThrowException(env, "java/lang/IllegalStateException", NULL);
        return;
    }
    process_media_player_call(env, thiz, mp->stop(), NULL, NULL);
}

static void luxuan_media_FFmpegMediaPlayer_pause(JNIEnv *env, jobject thiz)
{
    __android_log_print(ANDROID_LOG_VERBOSE, LOG_TAG, "pause");
    MediaPlayer* mp=getMediaPlayer(env, thiz);
    if(mp==NULL)
    {
        jniThrowException(env, "java/lang/IllegalStateException", NULL);
        return;
    }
    process_media_player_call(env, thiz, mp->pause(), NULL, NULL);
}

static jboolean luxuan_media_FFmpegMediaPlayer_isPlaying(JNIEnv *env, jobject thiz)
{
    MediaPlayer* mp=getMediaPlayer(env, thiz);
    if(mp==NULL)
    {
        jniThrowException(env, "java/lang/IllegalStateException", NULL);
        return false;
    }
    const jboolean is_playing=mp->isPlaying();

    __android_log_print(ANDROID_LOG_VERBOSE, LOG_TAG, "isPlaying: %d", is_playing);
    return is_playing;
}

static void luxuan_media_FFmpegMediaPlayer_seekTo(JNIEnv *env, jobject thiz, int msec)
{
    MediaPlayer* mp=getMediaPlayer(env, thiz);
    if(mp==NULL)
    {
        jniThrowException(env, "java/lang/IllegalStateException", NULL);
        return;
    }

    __android_log_print(ANDROID_LOG_VERBOSE, LOG_TAG, "seekTo: %d(msec)", msec);
    process_media_player_call(env, thiz, mp->seekTo(msec), NULL, NULL);
}

static int luxuan_media_FFmpegMediaPlayer_getVideoWidth(JNIEnv *env, jobject thiz)
{
    MediaPlayer* mp=getMediaPlayer(env, thiz);
    if(mp==NULL)
    {
        jniThrowException(env, "java/lang/IllegalStateException", NULL);
        return 0;
    }
    int w;
    if(0!=mp->getVideoWidth(&w))
    {
        __android_log_write(ANDROID_LOG_ERROR, LOG_TAG, "getVideoWidth failed");
        w=0;
    }
    __android_log_print(ANDROID_LOG_VERBOSE, LOG_TAG, "getVideoWidth: %d", w);
    return w;
}

static int luxuan_media_FFmpegMediaPlayer_getVideoHeight(JNIEnv *env, jobject thiz)
{
    MediaPlayer* mp=getMediaPlayer(env, thiz);
    if(mp==NULL)
    {
        jniThrowException(env, "java/lang/IllegalStateException", NULL);
        return 0;
    }
    int h;
    if(0!=mp->getVideoHeight(&h))
    {
        __android_log_write(ANDROID_LOG_ERROR, LOG_TAG, "getVideoHeight failed");
        h=0;
    }
    __android_log_print(ANDROID_LOG_VERBOSE, LOG_TAG, "getVideoHeight: %d", h);
    return h;
}

static int luxuan_media_FFmpegMediaPlayer_getCurrentPosition(JNIEnv *env, jobject thiz)
{

    MediaPlayer* mp=getMediaPlayer(env, thiz);
    if(mp==NULL)
    {
        jniThrowException(env, "java/lang/IllegalStateException", NULL);
        return 0;
    }
    int msec;
    process_media_player_call(env, thiz, mp->getCurrentPosition(&msec),NULL, NULL);
    __android_log_print(ANDROID_LOG_VERBOSE, LOG_TAG, "getCurrentPosition: %d (msec)", msec);
    return msec;
}

static int luxuan_media_FFmpegMediaPlayer_getDuration(JNIEnv *env, jobject thiz)
{
    MediaPlayer* mp=getMediaPlayer(env, thiz);
    if(mp==NULL)
    {
        jniThrowException(env, "java/lang/IllegalStateException", NULL);
        return 0;
    }
    int msec;
    process_media_player_call(env, thiz, mp->getDuration(&msec), NULL, NULL);
    __android_log_print(ANDROID_LOG_VERBOSE, LOG_TAG, "getDuration: %d (msec)", msec);
    return msec;
}

static void luxuan_media_FFmpegMediaPlayer_reset(JNIEnv *env, jobject thiz)
{
    __android_log_write(ANDROID_LOG_VERBOSE, LOG_TAG, "reset");
    MediaPlayer* mp=getMediaPlayer(env, thiz);
    if(mp==NULL)
    {
        jniThrowException(env, "java/lang/IllegalStateException", NULL);
        return;
    }
    process_media_player_call(env, thiz, mp->reset(), NULL, NULL);
}

static void luxuan_media_FFmpegMediaPlayer_setAudioStreamType(JNIEnv *env, jobject thiz, int streamtype)
{
    __android_log_print(ANDROID_LOG_VERBOSE, LOG_TAG, "setAudioStreamType: %s", streamtype);
    MediaPlayer* mp=getMediaPlayer(env, thiz);
    if(mp==NULL)
    {
        jniThrowException(env, "java/lang/IllegalStateException", NULL);
        return;
    }
    process_media_player_call(env, thiz, mp->setAudioStreamType(streamtype), NULL, NULL);
}

static void luxuan_media_FFmpegMediaPlayer_setLooping(JNIEnv *env, jobject thiz, jboolean looping)
{
    __android_log_print(ANDROID_LOG_VERBOSE, LOG_TAG, "setLooping: %d", looping);
    MediaPlayer* mp=getMediaPlayer(env, thiz);
    if(mp==NULL)
    {
        jniThrowException(env, "java/lang/IllegalStateException", NULL);
        return;
    }
    process_media_player_call(env, thiz, mp->setLooping(looping), NULL, NULL);
}

static jboolean luxuan_media_FFmpegMediaPlayer_isLooping(JNIEnv *env, jobject thiz)
{
    __android_log_write(ANDROID_LOG_VERBOSE, LOG_TAG, "isLooping");
    MediaPlayer* mp=getMediaPlayer(env, thiz);
    if(mp==NULL)
    {
        jniThrowException(env, "java/lang/IllegalStateException", NULL);
        return false;
    }
    return mp->isLooping();
}

static void luxuan_media_FFmpegMediaPlayer_setVolume(JNIEnv *env, jobject thiz, float leftVolume, float rightVolume)
{
    __android_log_print(ANDROID_LOG_VERBOSE, LOG_TAG, "setVolume: left: %f, right: %f", leftVolume, rightVolume);
    MediaPlayer* mp=getMediaPlayer(env, thiz);
    if(mp==NULL)
    {
        jniThrowException(env, "java/lang/IllegalStateException", NULL);
        return;
    }
    process_media_player_call(env, thiz, mp->setVolume(leftVolume, rightVolume),NULL,NULL);
}

static jint luxuan_media_FFmpegMediaPlayer_setMetadataFilter(JNIEnv *env, jobject thiz, jobjectArray allow, jobjectArray block)
{
    MediaPlayer* media_player=getMediaPlayer(env, thiz);
    if(media_player==NULL)
    {
        jniThrowException(env, "java/lang/IllegalStateException", NULL);
        return UNKNOWN_ERROR;
    }

    int allowCount=env->GetArrayLength(allow);
    char *allowed[allowCount];

    for(int i=0;i<allowCount;i++)
    {
        jstring allowString=(jstring)env->GetObjectArrayElement(allow, i);
        const char* rawAllowString=env->GetStringUTFChars(allowString, 0);
    }

    int blockCount=env->GetArrayLength(block);
    char* blocked[blockCount];

    for(int i=0;i<blockCount;i++)
    {
        jstring blockString=(jstring)env->GetObjectArrayElement(block,i);
        const char *rawBlockString=env->GetStringUTFChars(blockString, 0);
    }

    return 0;
}
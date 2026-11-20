#include "spatialpermission.h"

#if defined(__ANDROID__) && defined(OPENGOTHIC_XR)

#include <Tempest/AndroidApi>
#include <Tempest/Log>

#include <android/looper.h>
#include <android/native_activity.h>
#include <jni.h>
#include <memory>
#include <new>
#include <sys/eventfd.h>
#include <unistd.h>

namespace {
// NativeActivity loads the game's library in onCreate, on Android's UI thread.
// Keep that looper so the game thread can post a permission dialog to it.
struct UiLooper {
  ALooper* value = nullptr;
  UiLooper() {
    if(gettid()==getpid())
      value = ALooper_forThread();
    if(value!=nullptr)
      ALooper_acquire(value);
    }
  ~UiLooper() {
    if(value!=nullptr)
      ALooper_release(value);
    }
  } ui;

bool jniOk(JNIEnv* env) {
  if(!env->ExceptionCheck())
    return true;
  env->ExceptionClear();
  Tempest::Log::e("Android spatial permission call failed");
  return false;
  }

struct Request {
  JavaVM* vm;
  jobject activity;
  };

int requestOnUi(int fd, int events, void* data) {
  std::unique_ptr<Request> req(static_cast<Request*>(data));
  close(fd);
  JNIEnv* env = nullptr;
  // Android owns this thread and has already attached it to the VM.
  if(req->vm->GetEnv(reinterpret_cast<void**>(&env),JNI_VERSION_1_6)!=JNI_OK)
    return 0;
  if((events&ALOOPER_EVENT_INPUT)!=0 && env->PushLocalFrame(8)==JNI_OK) {
    auto activity = env->GetObjectClass(req->activity);
    auto alive = [&](const char* method) {
      auto id = env->GetMethodID(activity,method,"()Z");
      if(!jniOk(env) || id==nullptr)
        return false;
      const bool ended = env->CallBooleanMethod(req->activity,id)==JNI_TRUE;
      return jniOk(env) && !ended;
      };
    if(jniOk(env) && activity!=nullptr && alive("isFinishing") && alive("isDestroyed")) {
      auto str = env->FindClass("java/lang/String");
      auto perm = jniOk(env) && str ? env->NewStringUTF("com.oculus.permission.USE_SCENE") : nullptr;
      auto list = jniOk(env) && perm ? env->NewObjectArray(1,str,perm) : nullptr;
      auto id = jniOk(env) && list ? env->GetMethodID(activity,"requestPermissions","([Ljava/lang/String;I)V") : nullptr;
      if(jniOk(env) && id!=nullptr) {
        env->CallVoidMethod(req->activity,id,list,jint(73));
        if(jniOk(env))
          Tempest::Log::i("OpenXR: requested spatial permission for surface light");
        }
      }
    env->PopLocalFrame(nullptr);
    }
  jniOk(env);
  env->DeleteGlobalRef(req->activity);
  return 0; // remove this one-shot descriptor from the UI looper
  }
}

bool xrSpatialPermission(bool request) {
  auto* activity = Tempest::AndroidApi::activity();
  if(activity==nullptr)
    return false;
  auto* vm = activity->vm;
  JNIEnv* env = nullptr;
  const auto state = vm->GetEnv(reinterpret_cast<void**>(&env),JNI_VERSION_1_6);
  if(state!=JNI_OK && (state!=JNI_EDETACHED || vm->AttachCurrentThread(&env,nullptr)!=JNI_OK))
    return false;
  bool granted = false;
  if(env->PushLocalFrame(4)==JNI_OK) {
    auto cls = env->GetObjectClass(activity->clazz);
    auto perm = jniOk(env) && cls ? env->NewStringUTF("com.oculus.permission.USE_SCENE") : nullptr;
    auto id = jniOk(env) && perm ? env->GetMethodID(cls,"checkSelfPermission","(Ljava/lang/String;)I") : nullptr;
    if(jniOk(env) && id!=nullptr) {
      const jint result = env->CallIntMethod(activity->clazz,id,perm);
      if(jniOk(env)) {
        granted = result==0; // PackageManager.PERMISSION_GRANTED
        if(!granted && request && ui.value==nullptr) {
          static bool reported = false;
          if(!reported) {
            reported = true;
            Tempest::Log::e("Cannot request spatial permission: Android UI looper unavailable");
            }
          }
        if(!granted && request && ui.value!=nullptr) {
          auto* req = new(std::nothrow) Request{vm,nullptr};
          if(req) {
            req->activity = env->NewGlobalRef(activity->clazz);
            const int fd = jniOk(env) && req->activity ? eventfd(1,EFD_CLOEXEC|EFD_NONBLOCK) : -1;
            if(fd<0 || ALooper_addFd(ui.value,fd,ALOOPER_POLL_CALLBACK,ALOOPER_EVENT_INPUT,requestOnUi,req)!=1) {
              if(fd>=0)
                close(fd);
              env->DeleteGlobalRef(req->activity);
              delete req;
              Tempest::Log::e("Could not post the Android spatial permission request");
              }
            }
          }
        }
      }
    env->PopLocalFrame(nullptr);
    }
  jniOk(env);
  if(state==JNI_EDETACHED)
    vm->DetachCurrentThread();
  return granted;
  }

#else
bool xrSpatialPermission(bool) { return false; }
#endif

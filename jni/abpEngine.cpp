/*
 * This file is part of Adblock Plus <http://adblockplus.org/>,
 * Copyright (C) 2006-2013 Eyeo GmbH
 *
 * Adblock Plus is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Adblock Plus is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Adblock Plus.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <jni.h>
#include <AdblockPlus.h>
#include "AndroidLogSystem.h"
#include "AndroidWebRequest.h"
#include "Utils.h"
#include "debug.h"

JavaVM* globalJvm;
AdblockPlus::FilterEngine* filterEngine;
jobject jniObject;
bool manualUpdate = false;

extern "C"
{
  JNIEXPORT void JNICALL Java_org_adblockplus_android_ABPEngine_initialize(JNIEnv *pEnv, jobject, jstring basepath);
  JNIEXPORT void JNICALL Java_org_adblockplus_android_ABPEngine_release(JNIEnv *pEnv, jobject);
  JNIEXPORT jboolean JNICALL Java_org_adblockplus_android_ABPEngine_isFirstRun(JNIEnv *pEnv, jobject);
  JNIEXPORT jobjectArray JNICALL Java_org_adblockplus_android_ABPEngine_getListedSubscriptions(JNIEnv *pEnv, jobject);
  JNIEXPORT jobjectArray JNICALL Java_org_adblockplus_android_ABPEngine_getRecommendedSubscriptions(JNIEnv *pEnv, jobject);
  JNIEXPORT void JNICALL Java_org_adblockplus_android_ABPEngine_addSubscription(JNIEnv *pEnv, jobject, jstring url);
  JNIEXPORT void JNICALL Java_org_adblockplus_android_ABPEngine_removeSubscription(JNIEnv *pEnv, jobject, jstring url);
  JNIEXPORT void JNICALL Java_org_adblockplus_android_ABPEngine_refreshSubscription(JNIEnv *pEnv, jobject, jstring url);
  JNIEXPORT void JNICALL Java_org_adblockplus_android_ABPEngine_actualizeSubscriptionStatus(JNIEnv *pEnv, jobject, jstring url);
  JNIEXPORT jboolean JNICALL Java_org_adblockplus_android_ABPEngine_matches(JNIEnv *pEnv, jobject, jstring url, jstring contentType, jstring documentUrl);
  JNIEXPORT jobjectArray JNICALL Java_org_adblockplus_android_ABPEngine_getSelectorsForDomain(JNIEnv *pEnv, jobject, jstring domain);
  JNIEXPORT void JNICALL Java_org_adblockplus_android_ABPEngine_checkUpdates(JNIEnv *pEnv, jobject);
};

jobjectArray subscriptionsAsJavaArray(JNIEnv *pEnv, std::vector<AdblockPlus::SubscriptionPtr> subscriptions)
{
  D(D_WARN, "subscriptionsAsJavaArray()");
  static jclass cls = reinterpret_cast<jclass>(pEnv->NewGlobalRef(pEnv->FindClass("org/adblockplus/android/Subscription")));
  static jmethodID cid = pEnv->GetMethodID(cls, "<init>", "()V");
  static jfieldID ftitle = pEnv->GetFieldID(cls, "title", "Ljava/lang/String;");
  static jfieldID furl = pEnv->GetFieldID(cls, "url", "Ljava/lang/String;");

  D(D_WARN, "Subscriptions: %d", subscriptions.size());
  const jobjectArray ret = (jobjectArray) pEnv->NewObjectArray(subscriptions.size(), cls, NULL);

  int i = 0;
  for (std::vector<AdblockPlus::SubscriptionPtr>::const_iterator it = subscriptions.begin();
       it != subscriptions.end(); it++)
  {
    jobject subscription = pEnv->NewObject(cls, cid);
    pEnv->SetObjectField(subscription, ftitle, pEnv->NewStringUTF((*it)->GetProperty("title")->AsString().c_str()));
    pEnv->SetObjectField(subscription, furl, pEnv->NewStringUTF((*it)->GetProperty("url")->AsString().c_str()));
    pEnv->SetObjectArrayElement(ret, i, subscription);
    i++;
  }

  return ret;
}

void UpdateSubscriptionStatus(const AdblockPlus::SubscriptionPtr subscription)
{
  D(D_WARN, "UpdateSubscriptionStatus()");

  std::string downloadStatus = subscription->GetProperty("downloadStatus")->IsNull() ? "" : subscription->GetProperty("downloadStatus")->AsString();
  int64_t lastDownload = subscription->GetProperty("lastDownload")->AsInt();

  std::string status = "synchronize_never";
  int64_t time = 0;

  if (subscription->IsUpdating())
  {
    status = "synchronize_in_progress";
  }
  else if (!downloadStatus.empty() && downloadStatus != "synchronize_ok")
  {
    status = downloadStatus;
  }
  else if (lastDownload > 0)
  {
    time = lastDownload;
    status = "synchronize_last_at";
  }

  JNIEnv* jniEnv = NULL;
  int stat = globalJvm->GetEnv((void **)&jniEnv, JNI_VERSION_1_6);
  if (stat == JNI_EDETACHED)
  {
    if (globalJvm->AttachCurrentThread(&jniEnv, NULL) != 0)
      throw std::runtime_error("Failed to get JNI environment");
  }

  jstring jUrl = jniEnv->NewStringUTF(subscription->GetProperty("url")->AsString().c_str());
  jstring jStatus = jniEnv->NewStringUTF(status.c_str());
  jlong jTime = time * 1000;

  static jclass cls = jniEnv->GetObjectClass(jniObject);
  static jmethodID mid = jniEnv->GetMethodID(cls, "onFilterChanged", "(Ljava/lang/String;Ljava/lang/String;J)V");
  if (mid)
    jniEnv->CallVoidMethod(jniObject, mid, jUrl, jStatus, jTime);
  jniEnv->DeleteLocalRef(jUrl);
  jniEnv->DeleteLocalRef(jStatus);

  if (stat == JNI_EDETACHED)
    globalJvm->DetachCurrentThread();
}

void FilterChangedCallback(const std::string& action, const AdblockPlus::JsValuePtr item)
{
  D(D_WARN, "FilterChangedCallback()");

  if (action == "subscription.lastDownload" || action == "subscription.downloadStatus")
  {
    AdblockPlus::SubscriptionPtr subscription = AdblockPlus::SubscriptionPtr(new AdblockPlus::Subscription(item));
    UpdateSubscriptionStatus(subscription);
  }
}

void UpdateAvailableCallback(AdblockPlus::JsValueList& params)
{
  D(D_WARN, "UpdateAvailableCallback()");
  std::string updateUrl(params.size() >= 1 && !params[0]->IsNull() ? params[0]->AsString() : "");
  if (updateUrl.empty())
    return;

  JNIEnv* jniEnv = NULL;
  int stat = globalJvm->GetEnv((void **)&jniEnv, JNI_VERSION_1_6);
  if (stat == JNI_EDETACHED)
  {
    if (globalJvm->AttachCurrentThread(&jniEnv, NULL) != 0)
      throw std::runtime_error("Failed to get JNI environment");
  }

  jstring jUrl = jniEnv->NewStringUTF(updateUrl.c_str());

  static jclass cls = jniEnv->GetObjectClass(jniObject);
  static jmethodID mid = jniEnv->GetMethodID(cls, "onUpdateEvent", "(Ljava/lang/String;Ljava/lang/String;)V");
  if (mid)
    jniEnv->CallVoidMethod(jniObject, mid, jUrl, NULL);
  jniEnv->DeleteLocalRef(jUrl);

  if (stat == JNI_EDETACHED)
    globalJvm->DetachCurrentThread();
}

void UpdaterCallback(const std::string& error)
{
  JNIEnv* jniEnv = NULL;
  int stat = globalJvm->GetEnv((void **)&jniEnv, JNI_VERSION_1_6);
  if (stat == JNI_EDETACHED)
  {
    if (globalJvm->AttachCurrentThread(&jniEnv, NULL) != 0)
      throw std::runtime_error("Failed to get JNI environment");
  }

  static jclass cls = jniEnv->GetObjectClass(jniObject);
  static jmethodID mid = jniEnv->GetMethodID(cls, "onUpdateEvent", "(Ljava/lang/String;Ljava/lang/String;)V");

  if (!error.empty())
  {
    jstring jError = jniEnv->NewStringUTF(error.c_str());
    if (mid)
      jniEnv->CallVoidMethod(jniObject, mid, NULL, jError);
    jniEnv->DeleteLocalRef(jError);
  }
  else if (manualUpdate)
  {
    if (mid)
      jniEnv->CallVoidMethod(jniObject, mid, NULL, NULL);
  }

  if (stat == JNI_EDETACHED)
    globalJvm->DetachCurrentThread();
}

jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved)
{
  return JNI_VERSION_1_6;
}

void JNICALL JNI_OnUnload(JavaVM *vm, void *reserved)
{
}

JNIEXPORT void JNICALL Java_org_adblockplus_android_ABPEngine_initialize(JNIEnv *pEnv, jobject pObject, jstring basepath)
{
  D(D_WARN, "nativeInitialize()");
  int status = pEnv->GetJavaVM(&globalJvm);

  jniObject = pEnv->NewGlobalRef(pObject);

  const std::string path = GetString(pEnv, basepath);

  AdblockPlus::AppInfo appInfo;
  // TODO Should be extracted from the manifest
  appInfo.version = "1.1.2";
  appInfo.name = "adblockplusandroid";
  appInfo.application = "android";

  AdblockPlus::JsEnginePtr jsEngine(AdblockPlus::JsEngine::New(appInfo));

  AdblockPlus::DefaultFileSystem* defaultFileSystem = new AdblockPlus::DefaultFileSystem();
  AndroidLogSystem* androidLogSystem = new AndroidLogSystem();
  AndroidWebRequest* androidWebRequest = new AndroidWebRequest(globalJvm);

  defaultFileSystem->SetBasePath(path);
  jsEngine->SetLogSystem(AdblockPlus::LogSystemPtr(androidLogSystem));
  jsEngine->SetFileSystem(AdblockPlus::FileSystemPtr(defaultFileSystem));
  jsEngine->SetWebRequest(AdblockPlus::WebRequestPtr(androidWebRequest));
  jsEngine->SetEventCallback("updateAvailable", std::tr1::bind(&UpdateAvailableCallback, std::tr1::placeholders::_1));

  filterEngine = new AdblockPlus::FilterEngine(jsEngine);
  filterEngine->SetFilterChangeCallback(&FilterChangedCallback);
}

JNIEXPORT void JNICALL Java_org_adblockplus_android_ABPEngine_release(JNIEnv *pEnv, jobject)
{
  D(D_WARN, "nativeRelease()");
  AdblockPlus::JsEnginePtr jsEngine = filterEngine->GetJsEngine();
  jsEngine->RemoveEventCallback("updateAvailable");
  filterEngine->RemoveFilterChangeCallback();
  delete filterEngine;
  pEnv->DeleteGlobalRef(jniObject);
  jniObject = NULL;
  globalJvm = NULL;
}

JNIEXPORT jboolean JNICALL Java_org_adblockplus_android_ABPEngine_isFirstRun(JNIEnv *pEnv, jobject)
{
  return filterEngine->IsFirstRun() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jobjectArray JNICALL Java_org_adblockplus_android_ABPEngine_getListedSubscriptions(JNIEnv *pEnv, jobject)
{
  D(D_WARN, "getListedSubscriptions()");
  const std::vector<AdblockPlus::SubscriptionPtr> subscriptions = filterEngine->GetListedSubscriptions();
  return subscriptionsAsJavaArray(pEnv, subscriptions);
}

JNIEXPORT jobjectArray JNICALL Java_org_adblockplus_android_ABPEngine_getRecommendedSubscriptions(JNIEnv *pEnv, jobject)
{
  D(D_WARN, "getRecommendedSubscriptions()");
  try
  {
    const std::vector<AdblockPlus::SubscriptionPtr> subscriptions = filterEngine->FetchAvailableSubscriptions();
    return subscriptionsAsJavaArray(pEnv, subscriptions);
  }
  catch (const std::exception& e)
  {
    D(D_ERROR, "Exception: %s", e.what());
  }
  catch (...)
  {
    D(D_ERROR, "Unknown exception");
  }
  return NULL;
}

JNIEXPORT void JNICALL Java_org_adblockplus_android_ABPEngine_addSubscription(JNIEnv *pEnv, jobject, jstring url)
{
  D(D_WARN, "addSubscription()");
  const std::string surl = GetString(pEnv, url);
  AdblockPlus::SubscriptionPtr subscription = filterEngine->GetSubscription(surl);
  subscription->AddToList();
}

JNIEXPORT void JNICALL Java_org_adblockplus_android_ABPEngine_removeSubscription(JNIEnv *pEnv, jobject, jstring url)
{
  D(D_WARN, "removeSubscription()");
  const std::string surl = GetString(pEnv, url);
  AdblockPlus::SubscriptionPtr subscription = filterEngine->GetSubscription(surl);
  if (subscription->IsListed())
  {
    subscription->RemoveFromList();
  }
}

JNIEXPORT void JNICALL Java_org_adblockplus_android_ABPEngine_refreshSubscription(JNIEnv *pEnv, jobject, jstring url)
{
  D(D_WARN, "refreshSubscription()");
  const std::string surl = GetString(pEnv, url);
  AdblockPlus::SubscriptionPtr subscription = filterEngine->GetSubscription(surl);
  subscription->UpdateFilters();
}

JNIEXPORT void JNICALL Java_org_adblockplus_android_ABPEngine_actualizeSubscriptionStatus(JNIEnv *pEnv, jobject, jstring url)
{
  D(D_WARN, "actualizeSubscriptionStatus()");
  const std::string surl = GetString(pEnv, url);
  AdblockPlus::SubscriptionPtr subscription = filterEngine->GetSubscription(surl);
  UpdateSubscriptionStatus(subscription);
}

JNIEXPORT jboolean JNICALL Java_org_adblockplus_android_ABPEngine_matches(JNIEnv *pEnv, jobject, jstring url, jstring contentType, jstring documentUrl)
{
  const std::string surl = GetString(pEnv, url);
  const std::string stype = GetString(pEnv, contentType);
  const std::string sdoc = GetString(pEnv, documentUrl);

  AdblockPlus::FilterPtr filter = filterEngine->Matches(surl, stype, sdoc);

  if (! filter)
    return JNI_FALSE;

  // hack: if there is no referrer block only if filter is domain-specific
  // (to re-enable in-app ads blocking, proposed on 12.11.2012 Monday meeting)
  // documentUrl contains the referrer (Android application special case)
  if (sdoc.empty() && (filter->GetProperty("text")->AsString()).find("||") != std::string::npos)
    return JNI_FALSE;

  return filter->GetType() == AdblockPlus::Filter::TYPE_EXCEPTION ? JNI_FALSE : JNI_TRUE;
}

JNIEXPORT jobjectArray JNICALL Java_org_adblockplus_android_ABPEngine_getSelectorsForDomain(JNIEnv *pEnv, jobject, jstring domain)
{
  const std::string sdomain = GetString(pEnv, domain);
  const std::vector<std::string> selectors = filterEngine->GetElementHidingSelectors(sdomain);

  static jclass cls = reinterpret_cast<jclass>(pEnv->NewGlobalRef(pEnv->FindClass("java/lang/String")));

  D(D_WARN, "Selectors: %d", selectors.size());
  const jobjectArray ret = (jobjectArray) pEnv->NewObjectArray(selectors.size(), cls, NULL);

  int i = 0;
  for (std::vector<std::string>::const_iterator it = selectors.begin();
       it != selectors.end(); it++)
  {
    jstring selector = pEnv->NewStringUTF((*it).c_str());
    pEnv->SetObjectArrayElement(ret, i, selector);
    pEnv->DeleteLocalRef(selector);
    i++;
  }

  return ret;
}

JNIEXPORT void JNICALL Java_org_adblockplus_android_ABPEngine_checkUpdates(JNIEnv *pEnv, jobject)
{
  manualUpdate = true;
  filterEngine->ForceUpdateCheck(UpdaterCallback);
}

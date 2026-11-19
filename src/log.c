#include "log.h"
#include <stdarg.h>
#include <stdio.h>
#ifdef __ANDROID__
#include <android/log.h>
#endif

void nzm_log(const char *format, ...) {
  char message[2048];
  va_list args;
  va_start(args, format);
  int n=vsnprintf(message,sizeof message,format,args);
  va_end(args);
  if(n<0)return;
  fprintf(stderr,"%s\n",message);
#ifdef __ANDROID__
  __android_log_write(ANDROID_LOG_INFO,"lge-zramd",message);
#endif
}

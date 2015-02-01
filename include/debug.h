/*
 * Copyright 2013  Samsung Electronics Co., Ltd
 *
 * Licensed under the Flora License, Version 1.1 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://floralicense.org/license/
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#if !defined(FLOG)
#define DbgPrint(format, arg...)	SECURE_LOGD(format, ##arg)
#define ErrPrint(format, arg...)	SECURE_LOGE(format, ##arg)
#define WarnPrint(format, arg...)	SECURE_LOGW(format, ##arg)
#else
extern FILE *__file_log_fp;
#define DbgPrint(format, arg...) do { fprintf(__file_log_fp, "[LOG] [[32m%s/%s[0m:%d] " format, util_basename(__FILE__), __func__, __LINE__, ##arg); fflush(__file_log_fp); } while (0)
#define ErrPrint(format, arg...) do { fprintf(__file_log_fp, "[ERR] [[32m%s/%s[0m:%d] " format, util_basename(__FILE__), __func__, __LINE__, ##arg); fflush(__file_log_fp); } while (0)
#define WarnPrint(format, arg...) do { fprintf(__file_log_fp, "[WRN] [[32m%s/%s[0m:%d] " format, util_basename(__FILE__), __func__, __LINE__, ##arg); fflush(__file_log_fp); } while (0)
#endif

// DbgPrint("FREE\n");
#define DbgFree(a) do { \
	free(a); \
} while (0)

#define DbgXFree(a) do { \
	DbgPrint("XFree\n"); \
	XFree(a); \
} while (0)


#if defined(LOG_TAG)
#undef LOG_TAG
#endif

#define LOG_TAG "DATA_PROVIDER_MASTER"

#if defined(_ENABLE_PERF)
#define PERF_INIT() \
	struct timeval __stv; \
	struct timeval __etv; \
	struct timeval __rtv

#define PERF_BEGIN() do { \
	if (gettimeofday(&__stv, NULL) < 0) { \
		ErrPrint("gettimeofday: %s\n", strerror(errno)); \
	} \
} while (0)

#define PERF_MARK(tag) do { \
	if (gettimeofday(&__etv, NULL) < 0) { \
		ErrPrint("gettimeofday: %s\n", strerror(errno)); \
	} \
	timersub(&__etv, &__stv, &__rtv); \
	DbgPrint("[%s] %u.%06u\n", tag, __rtv.tv_sec, __rtv.tv_usec); \
} while (0)
#else
#define PERF_INIT()
#define PERF_BEGIN()
#define PERF_MARK(tag)
#endif

#define HAPI __attribute__((visibility("hidden")))

/* End of a file */

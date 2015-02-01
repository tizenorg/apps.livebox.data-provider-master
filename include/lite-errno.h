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

#ifndef __DYNAMICBOX_ERRNO_H
#define __DYNAMICBOX_ERRNO_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @addtogroup CAPI_DYNAMICBOX_SERVICE_MODULE
 * @{
 */

/**
 * @brief
 * Definitions for the result status of dynamicbox operation.
 */
enum dynamicbox_status {
	DBOX_STATUS_ERROR_NONE = 0x00000000, /**< Operation is successfully completed */
	DBOX_STATUS_ERROR = 0x80000000, /**< This will be OR'd with other specific error value */
	DBOX_STATUS_ERROR_INVALID_PARAMETER = DBOX_STATUS_ERROR | 0x0001, /**< Invalid request */
	DBOX_STATUS_ERROR_FAULT = DBOX_STATUS_ERROR | 0x0002, /**< Fault - Unable to recover from the error */
	DBOX_STATUS_ERROR_OUT_OF_MEMORY = DBOX_STATUS_ERROR | 0x0004, /**< Memory is not enough to do this operation */
	DBOX_STATUS_ERROR_EXIST = DBOX_STATUS_ERROR | 0x0008, /**< Already exists */
	DBOX_STATUS_ERROR_BUSY = DBOX_STATUS_ERROR | 0x0010, /**< Busy so the operation is not started(accepted), try again */
	DBOX_STATUS_ERROR_PERMISSION_DENIED = DBOX_STATUS_ERROR | 0x0020, /**< Permission error */
	DBOX_STATUS_ERROR_ALREADY = DBOX_STATUS_ERROR | 0x0040, /**< Operation is already started */
	DBOX_STATUS_ERROR_CANCEL = DBOX_STATUS_ERROR | 0x0080, /**< Operation is canceled */
	DBOX_STATUS_ERROR_IO_ERROR = DBOX_STATUS_ERROR | 0x0100, /**< I/O Error */
	DBOX_STATUS_ERROR_NOT_EXIST = DBOX_STATUS_ERROR | 0x0200, /**< Not exists */
	DBOX_STATUS_ERROR_TIMEOUT = DBOX_STATUS_ERROR | 0x0400, /**< Timeout */
	DBOX_STATUS_ERROR_NOT_IMPLEMENTED = DBOX_STATUS_ERROR | 0x0800, /**< Operation is not implemented */
	DBOX_STATUS_ERROR_NO_SPACE = DBOX_STATUS_ERROR | 0x1000, /**< No space to operate */
	DBOX_STATUS_ERROR_DISABLED = DBOX_STATUS_ERROR | 0x2000 /**< Disabled */
};


/*!
 * \brief Check whether given code value indicates error or not.
 * \param[in] s
 * \return 1 or 0
 */
#define DBOX_STATUS_IS_ERROR(s)	(!!((s) & DBOX_STATUS_ERROR))

/*!
 * \}
 */

#ifdef __cplusplus
}
#endif

#endif
/* End of a file */


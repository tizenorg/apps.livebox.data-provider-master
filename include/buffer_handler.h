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

struct buffer_info;
struct inst_info;
struct buffer;

#include <dynamicbox_buffer.h>

/*!
 * \brief
 * \param[in] type
 * \param[in] w
 * \param[in] h
 * \param[in] pixel_size
 * \return buffer_info
 */
extern struct buffer_info *buffer_handler_create(struct inst_info *inst, enum dynamicbox_fb_type type, int w, int h, int pixel_size);

/*!
 * \brief
 * \param[in] info
 * \return int
 */
extern int buffer_handler_destroy(struct buffer_info *info);

/*!
 * \brief
 * \param[in] info
 * \return int
 */
extern int buffer_handler_load(struct buffer_info *info);

/*!
 * \brief
 * \param[in] info
 * \return int
 */
extern int buffer_handler_unload(struct buffer_info *info);

/*!
 * \brief
 * \param[in] info
 * \return int
 */
extern int buffer_handler_is_loaded(const struct buffer_info *info);

/*!
 * \brief Reallocate buffer
 * \param[in] info
 * \param[in] w
 * \param[in] h
 * \return int
 */
extern int buffer_handler_resize(struct buffer_info *info, int w, int h);

/*!
 * \brief Only update the size information
 * \param[in] info
 * \param[in] w
 * \param[in] h
 * \return void
 */
extern void buffer_handler_update_size(struct buffer_info *info, int w, int h);

/*!
 * \brief
 * \param[in] info
 * \return const char *
 */
extern const char *buffer_handler_id(const struct buffer_info *info);

/*!
 * \param[in] info
 * \return dynamicbox_fb_type
 */
extern enum dynamicbox_fb_type buffer_handler_type(const struct buffer_info *info);

/*!
 * \brief This API is not supported for Pixmap.
 * \param[in] info
 * \return void*
 */
extern void *buffer_handler_fb(struct buffer_info *info);

/*!
 * \brief
 * \param[in] info
 * \param[out] w
 * \param[out] h
 * \return int
 */
extern int buffer_handler_get_size(struct buffer_info *info, int *w, int *h);

/*!
 * \brief This API only can be used for file type buffer
 * \param[in] info
 * \return void
 */
extern void buffer_handler_flush(struct buffer_info *info);

/*!
 * \brief
 * \param[in] info
 * \return 0 if fails. Return value should be casted to Pixmap type
 */
extern int buffer_handler_pixmap(const struct buffer_info *info);

/*!
 * \brief
 * \param[in] info
 * \return buffer
 */
extern void *buffer_handler_pixmap_acquire_buffer(struct buffer_info *info);

/*!
 * \brief
 * \param[in] info
 * \return int
 */
extern int buffer_handler_pixmap_release_buffer(void *canvas);

/*!
 * \brief
 * \param[in] info
 * \return int
 */
extern int buffer_handler_stride(struct buffer_info *info);

/*!
 * \brief
 * \param[in] info
 * \return int
 */
extern int buffer_handler_pixels(struct buffer_info *info);

/*!
 * \brief
 * \param[in] info
 * \return int
 */
extern int buffer_handler_auto_align(void);

/*!
 * \brief
 * \return int
 */
extern int buffer_handler_init(void);

/*!
 * \brief
 * \details
 * \remarks
 * \return int
 * \retval
 * \pre
 * \post
 * \sa
 */
extern int buffer_handler_fini(void);

/*!
 * \brief
 * \details
 * \remarks
 * \param[in] info
 * \return void *
 * \retval NULL
 * \retval address
 * \pre
 * \post
 * \sa
 */
extern void *buffer_handler_pixmap_ref(struct buffer_info *info);

/*!
 * \brief
 * \details
 * \remarks
 * \param[in] buffer_ptr
 * \return int
 * \retval 
 * \pre
 * \post
 * \sa
 */
extern int buffer_handler_pixmap_unref(void *buffer_ptr);

/*!
 * \brief
 * \details
 * \remarks
 * \param[in] pixmap
 * \return void *
 * \retval
 * \pre
 * \post
 * \sa
 */
extern void *buffer_handler_pixmap_find(int pixmap);

/*!
 * \brief
 * \details
 * \remarks
 * \param[in] info
 * \return void *
 * \retval
 * \pre
 * \post
 * \sa
 */
extern void *buffer_handler_pixmap_buffer(struct buffer_info *info);

/*!
 * \brief
 * \details
 * \remarks
 * \param[in] info
 * \return struct inst_info *
 * \retval
 * \pre
 * \post
 * \sa
 */
extern struct inst_info *buffer_handler_instance(struct buffer_info *info);

/*!
 * \brief
 * \details
 * \remarks
 * \param[in] type
 * \param[in] resource
 * \return dynamicbox_fb_t 
 * \retval NULL
 * \retval address
 * \pre
 * \post
 * \sa
 */
extern dynamicbox_fb_t buffer_handler_raw_open(enum dynamicbox_fb_type type, void *resource);

/*!
 * \brief
 * \details
 * \remarks
 * \param[in] buffer
 * \return int
 * \retval
 * \pre
 * \post
 * \sa
 */
extern int buffer_handler_raw_close(dynamicbox_fb_t buffer);

/*!
 * \brief
 * \details
 * \remarks
 * \param[in] buffer
 * \return void *
 * \retval NULL
 * \pre
 * \post
 * \sa
 */
extern void *buffer_handler_raw_data(dynamicbox_fb_t buffer);

/*!
 * \brief
 * \details
 * \remarks
 * \param[in] buffer
 * \return int
 * \retval
 * \pre
 * \post
 * \sa
 */
extern int buffer_handler_raw_size(dynamicbox_fb_t buffer);

/*!
 * \brief
 * \details
 * \remarks
 * \param[in] buffer
 * \return int
 * \retval
 * \pre
 * \post
 * \sa
 */
extern int buffer_handler_lock(struct buffer_info *buffer);

/*!
 * \brief
 * \details
 * \remarks
 * \param[in] buffer
 * \return int
 * \retval
 * \pre
 * \post
 * \sa
 */
extern int buffer_handler_unlock(struct buffer_info *buffer);

/*!
 * \brief
 * \details
 * \remarks
 * \param[in] buffer
 * \param[in] data
 * \return int
 * \retval
 * \pre
 * \post
 * \sa
 */
extern int buffer_handler_set_data(struct buffer_info *buffer, void *data);

/*!
 * \brief
 * \details
 * \remarks
 * \param[in] buffer
 * \return void *
 * \retval NULL
 * \retval address
 * \pre
 * \post
 * \sa
 */
extern void *buffer_handler_data(struct buffer_info *buffer);
/* End of a file */

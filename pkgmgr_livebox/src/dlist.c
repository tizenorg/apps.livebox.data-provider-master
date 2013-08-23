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

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include "dlist.h"

/*!
 * \brief
 * This dlist is called Modified Doubly Linked List.
 *
 * Noramlly, The dobule linked list contains address of previous and next element.
 * This dlist also contains them, but the tail element only contains prev address.
 *
 * The head element's prev pointer indicates the last element.
 * But the last element's next pointer indicates NIL.
 *
 * So we can find the last element while crawling this DList
 * But we have to remember the address of the head element.
 */

struct dlist {
	struct dlist *next;
	struct dlist *prev;
	void *data;
};

struct dlist *dlist_append(struct dlist *list, void *data)
{
	struct dlist *item;

	item = malloc(sizeof(*item));
	if (!item) {
		return NULL;
	}

	item->next = NULL;
	item->data = data;

	if (!list) {
		item->prev = item;

		list = item;
	} else {
		item->prev = list->prev;
		item->prev->next = item;
		list->prev = item;
	}

	assert(!list->prev->next && "item NEXT");

	return list;
}

struct dlist *dlist_prepend(struct dlist *list, void *data)
{
	struct dlist *item;

	item = malloc(sizeof(*item));
	if (!item) {
		return NULL;
	}

	item->data = data;

	if (!list) {
		item->prev = item;
		item->next = NULL;
	} else {
		if (list->prev->next) {
			list->prev->next = item;
		}

		item->prev = list->prev;
		item->next = list;

		list->prev = item;

	}

	return item;
}

struct dlist *dlist_remove(struct dlist *list, struct dlist *l)
{
	if (!list || !l) {
		return NULL;
	}

	if (l == list) {
		list = l->next;
	} else {
		l->prev->next = l->next;
	}

	if (l->next) {
		l->next->prev = l->prev;
	}
	/*!
	 * \note
	 * If the removed entry 'l' has no next element, it is the last element.
	 * In this case, check the existence of the list first,
	 * and if the list is not empty, update the 'prev' of the list (which is a head element of the list) 
	 *
	 * If we didn't care about this, the head element(list) can indicates the invalid element.
	 */
	else if (list) {
		list->prev = l->prev;
	}

	free(l);
	return list;
}

struct dlist *dlist_find_data(struct dlist *list, void *data)
{
	struct dlist *l;
	void *_data;

	dlist_foreach(list, l, _data) {
		if (data == _data) {
			return l;
		}
	}

	return NULL;
}

void *dlist_data(struct dlist *l)
{
	return l ? l->data : NULL;
}

struct dlist *dlist_next(struct dlist *l)
{
	return l ? l->next : NULL;
}

struct dlist *dlist_prev(struct dlist *l)
{
	return l ? l->prev : NULL;
}

int dlist_count(struct dlist *l)
{
	register int i;
	struct dlist *n;
	void *data;

	i = 0;
	dlist_foreach(l, n, data) {
		i++;
	}

	return i;
}

struct dlist *dlist_nth(struct dlist *l, int nth)
{
	register int i;
	struct dlist *n;

	i = 0;
	for (n = l; n; n = n->next) {
		if (i == nth) {
			return n;
		}
		i++;
	}

	return NULL;
}

/* End of a file */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "node.h"

struct node {
	char *name;
	enum node_type type;

	void *data;

	struct node *parent;
	unsigned char mode;

	struct {
		struct node *next;
		struct node *prev;
	} sibling;

	struct node *child;
};

int errno; /* External symbol */

char *node_to_abspath(struct node *node)
{
	char *path;
	char *ptr;
	struct node *tmp;
	int len = 0;

	tmp = node;
	while (tmp && node_name(tmp)) {
		len += strlen(node_name(tmp)) + 1; /* trail '/' */
		tmp = node_parent(tmp);
	}

	path = malloc(len + 2); /* '/' and '\0' */
	if (!path)
		return NULL;

	if (!len) {
		path[0] = '/';
		path[1] = '\0';
	} else {
		ptr = path + len;
		*ptr = '\0';
		tmp = node;
		while (tmp && node_name(tmp)) {
			ptr -= (strlen(node_name(tmp)) + 1);
			*ptr = '/';
			strncpy(ptr + 1, node_name(tmp), strlen(node_name(tmp)));
			tmp = node_parent(tmp);
		}
	}

	return path;
}

static inline int next_state(int from, char ch)
{
	switch ( ch )
	{
		case '/':
			return 1;
		case '.':
			if ( from == 1 ) return 2;
			if ( from == 2 ) return 3;
	}

	return 4;
}

static inline void abspath(char* pBuffer, char* pRet)
{
	int idx=0;
	int state = 1;
	int from;
	int src_idx = 0;
	int src_len = strlen(pBuffer);
	pRet[idx] = '/';
	idx ++;

	while (src_idx <= src_len) {
		from = state;
		state = next_state(from, pBuffer[src_idx]);

		switch (from) {
			case 1:
				if ( state != 1 ) {
					pRet[idx] = pBuffer[src_idx];
					idx ++;
				}
				break;
			case 2:
				if ( state == 1 ) {
					if ( idx > 0 ) idx --;
				} else {
					pRet[idx] = pBuffer[src_idx];
					idx ++;
				}
				break;
			case 3:
				// Only can go to the 1 or 4
				if ( state == 1 ) {
					idx -= 2;
					if ( idx < 0 ) idx = 0;

					while ( idx > 0 && pRet[idx] != '/' ) idx --;
					if ( idx > 0 && pRet[idx] == '/' ) idx --;
					while ( idx > 0 && pRet[idx] != '/' ) idx --;
				}
			case 4:
				pRet[idx] = pBuffer[src_idx];
				idx ++;
				break;
		}

		pRet[idx] = '\0';
		src_idx ++;
	}
}

struct node *node_find(const struct node *node, char *path)
{
	int len;
	char *ptr;
	char *buffer;

	buffer = malloc(strlen(path) + 3); /* add 2 more bytes */
	if (!buffer)
		return NULL;

	abspath(path, buffer);
	ptr = buffer;

	do {
		ptr += (*ptr == '/');
		for (len = 0; ptr[len] && ptr[len] != '/'; len++);
		if (!len)
			break;

		if (!strncmp("..", ptr, len)) {
			ptr += len;
			node = node->parent ? node->parent : node;
			continue;
		}

		if (!strncmp(".", ptr, len)) {
			ptr += len;
			continue;
		}

		node = node->child;
		if (!node) {
			free(buffer);
			return NULL;
		}

		while (node) {
			if (!strncmp(node->name, ptr, len) && node->name[len] == '\0') {
				ptr += len;
				break;
			}

			node = node->sibling.next;
		}
	} while (*ptr && node);

	free(buffer);
	return (struct node *)node;
}

struct node *node_create(struct node *parent, const char *name, enum node_type type)
{
	struct node *node;

	node = malloc(sizeof(*node));
	if (!node) {
		printf("Error: %s\n", strerror(errno));
		return NULL;
	}

	node->parent = parent;

	if (name) {
		node->name = strdup(name);
		if (!node->name) {
			printf("Error: %s\n", strerror(errno));
			return NULL;
		}
	} else {
		node->name = NULL;
	}

	node->type = type;

	node->sibling.next = NULL;
	node->sibling.prev = NULL;

	node->child = NULL;
	node->data = NULL;

	if (parent) {
		if (parent->child) {
			struct node *tmp;
			tmp = parent->child;
			while (tmp->sibling.next)
				tmp = tmp->sibling.next;

			tmp->sibling.next = node;
			node->sibling.prev = tmp;
		} else {
			parent->child = node;
		}
	}
	return node;
}

void node_destroy(struct node *node)
{
	free(node->name);
	free(node);
}

struct node * const node_next_sibling(const struct node *node)
{
	return node->sibling.next;
}

struct node * const node_prev_sibling(const struct node *node)
{
	return node->sibling.prev;
}

void node_set_mode(struct node *node, int mode)
{
	node->mode = mode;
}

void node_set_data(struct node *node, void *data)
{
	node->data = data;
}

void node_set_type(struct node *node, enum node_type type)
{
	node->type = type;
}

struct node * const node_child(const struct node *node)
{
	return node->child;
}

struct node * const node_parent(const struct node *node)
{
	return node->parent;
}

const int const node_mode(const struct node *node)
{
	return node->mode;
}

void * const node_data(const struct node *node)
{
	return node->data;
}

const enum node_type const node_type(const struct node *node)
{
	return node->type;
}

const char * const node_name(const struct node *node)
{
	return node->name;
}

/* End of a file */

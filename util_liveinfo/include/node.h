
enum node_type {
	NODE_DIR,
	NODE_FILE,
	NODE_LINK,
};

struct node;

#define NODE_READ   0x01
#define NODE_WRITE  0x02
#define NODE_EXEC   0x04

extern struct node *node_find(const struct node *node, const char *path);
extern struct node *node_create(struct node *parent, const char *name, enum node_type type, int mode);
extern void *node_destroy(struct node *node);

extern struct node * const node_next_sibling(const struct node *node);
extern struct node * const node_prev_sibling(const struct node *node);

extern struct node * const node_child(const struct node *node);
extern struct node * const node_parent(const struct node *node);

extern void node_set_mode(struct node *node, int mode);
extern void node_set_data(struct node *node, void *data);

extern const int const node_mode(const struct node *node);
extern void * const node_data(const struct node *node);

extern void node_set_type(struct node *node, enum node_type type);
extern const enum node_type const node_type(const struct node *node);

extern const char * const node_name(const struct node *node);

extern char *node_to_abspath(const struct node *node);

extern int node_age(struct node *node);
extern void node_set_age(struct node *node, int age);

extern void node_delete(struct node *node, void (del_cb)(struct node *node));
/* End of a file */

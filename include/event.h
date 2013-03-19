struct event_data {
	int x;
	int y;
	int device;
};

enum event_state {
	EVENT_STATE_ACTIVATE,
	EVENT_STATE_ACTIVATED,
	EVENT_STATE_DEACTIVATE,
};

extern int event_init(void);
extern int event_fini(void);
extern int event_activate(int x, int y, int (*event_cb)(enum event_state state, struct event_data *event, void *data), void *data);
extern int event_deactivate(void);
extern int event_is_activated(void);

/* End of a file */

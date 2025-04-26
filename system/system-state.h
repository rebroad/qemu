#ifndef SYSTEM_STATE_H
#define SYSTEM_STATE_H

#include "cpu-stats.h"

/* System state flags */
#define STATE_UNKNOWN     0
#define STATE_PROM_IDLE   1
#define STATE_OS_IDLE     2
#define STATE_SHUTDOWN    3

/* Operation modes */
#define MODE_TRAINING     1
#define MODE_DETECTION    2
#define MODE_DISABLED     3

/* Confidence tracking */
struct func_confidence {
    float timing_match;     /* How well current timings match learned patterns */
    float streak_match;     /* How well current streaks match learned patterns */
    float overall;         /* Combined confidence score */
};

/* Function-specific timing and stats data */
struct func_stats {
    int func_id;          /* ID of the function these stats are for */
    /* Call counts from last sample */
    unsigned int true_count;
    unsigned int false_count;
    /* Timing stats */
    uint64_t min_true_ns;
    uint64_t max_true_ns;
    uint64_t avg_true_ns;
    uint64_t min_false_ns;
    uint64_t max_false_ns;
    uint64_t avg_false_ns;
    /* Streak tracking */
    unsigned int min_true_streak;
    unsigned int max_true_streak;
    unsigned int min_false_streak;
    unsigned int max_false_streak;
    /* Confidence tracking */
    struct func_confidence confidence;
};

/* Training data for each state */
struct state_training_data {
    struct func_stats *func_stats;  /* Array of per-function statistics */
    unsigned int num_funcs;         /* Number of functions we're tracking */
    float overall_confidence;       /* Combined confidence across all functions */
    bool learning_complete;         /* Whether we've gathered enough data for this state */
};

/* Initialize state detection */
void system_state_init(void);

/* QMP command handlers */
bool qmp_set_system_state(int state, int mode, Error **errp);

/* Get current operating mode */
int system_state_get_mode(void);

/* Set the current system state (for training) */
void system_state_set_training_state(int state);

/* Load previously saved training data from a configuration file */
int system_state_load_config(const char *filename);

/* Save current training data for future use */
int system_state_save_config(const char *filename);

/* Update state detection based on current debug statistics */
int system_state_update(const struct debug_counters *counters);

/* Get current detected state */
int system_state_get(void);

/* Register a callback for state changes */
typedef void (*state_change_cb)(int old_state, int new_state, void *data);
void system_state_register_callback(state_change_cb cb, void *data);

/* Get training statistics for a given state */
const struct state_training_data *system_state_get_training_data(int state);

/* Print current training data statistics */
void system_state_print_stats(void);

#endif /* SYSTEM_STATE_H */

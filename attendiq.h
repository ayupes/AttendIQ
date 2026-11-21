#ifndef ATTENDIQ_H
#define ATTENDIQ_H

#include <curl/curl.h>

#define MAX_SUBJECTS 16
#define MAX_SESSIONS 256
#define TOKEN_SIZE 4096
#define ID_SIZE 64

struct SlotDedupe {
    int date_int;
    char start_time[16];
};

struct RecordedSession {
    int date_int;
    char time_str[32];
    int start_min;
    char status[16];
    int matched;
};

struct PendingClass {
    int date_int;
    char date_str[16];
    char time_str[32];
    int user_choice; // 0 = Unset, 1 = YES (Attended), 2 = NO (Missed)
};

struct Subject {
    char code[32];
    char name[128];
    int present;
    int absent;
    int condoned;
    int held;
    double percentage;

    // Recorded sessions from AttendanceDetails
    struct RecordedSession sessions[MAX_SESSIONS];
    int session_count;

    // Timetable deduplication
    struct SlotDedupe seen_slots[MAX_SESSIONS];
    int seen_slot_count;

    // Timetable counts
    int future_classes;
    struct PendingClass pending_items[16];
    int pending_classes;

    // Projections
    int needed_for_75;
    int safe_to_skip;
    double max_achievable;
    double simulated_pct;
};

/* Authenticates with MyUPES and returns access token and student GUID */
int authenticate(CURL *curl, const char *username, const char *password,
                 char *access_token, char *student_id);

/* Fetches live attendance records for all subjects */
int fetch_attendance(CURL *curl, const char *access_token, const char *student_id,
                     struct Subject *subjects, int *subject_count);

/* Fetches timetable and matches with attendance records */
int fetch_timetable(CURL *curl, const char *access_token, const char *student_id,
                    struct Subject *subjects, int subject_count);

/* Recalculates metrics for a subject */
void calculate_metrics(struct Subject *s);

#endif

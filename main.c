#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <curl/curl.h>
#include "attendiq.h"

static void read_input(char *buffer, size_t size, int hide) {
    struct termios oldt, newt;
    int tty = isatty(STDIN_FILENO);

    if (hide && tty) {
        tcgetattr(STDIN_FILENO, &oldt);
        newt = oldt;
        newt.c_lflag &= ~ECHO;
        tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    }

    if (fgets(buffer, (int)size, stdin)) {
        size_t len = strlen(buffer);
        while (len > 0 && (buffer[len - 1] == '\n' || buffer[len - 1] == '\r')) {
            buffer[--len] = '\0';
        }
    }

    if (hide && tty) {
        tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
        printf("\n");
    }
}

static void print_attendance_table(struct Subject *subjects, int count) {
    printf("\n===============================================================================================================\n");
    printf("%-3s | %-32s | %-7s | %-5s | %-10s | %-7s | %-6s | %-15s | %-11s\n",
           "#", "Subject Name", "Present", "Held", "Official %", "Pending", "Future", "75% Target", "Max Final %");
    printf("===============================================================================================================\n");

    for (int i = 0; i < count; i++) {
        char name_buf[33] = {0};
        strncpy(name_buf, subjects[i].name, 32);

        char target_buf[20] = {0};
        if (subjects[i].percentage >= 75.0) {
            snprintf(target_buf, sizeof(target_buf), "Safe (skip %d)", subjects[i].safe_to_skip);
        } else if (subjects[i].needed_for_75 <= subjects[i].future_classes + subjects[i].pending_classes) {
            snprintf(target_buf, sizeof(target_buf), "Need %d class(es)", subjects[i].needed_for_75);
        } else {
            snprintf(target_buf, sizeof(target_buf), "Unachievable");
        }

        // Show simulated percentage indicator if temporary adjustment is active
        int has_adj = 0;
        for (int k = 0; k < subjects[i].pending_classes; k++) {
            if (subjects[i].pending_items[k].user_choice > 0) {
                has_adj = 1;
                break;
            }
        }

        char pct_str[24];
        if (has_adj) {
            snprintf(pct_str, sizeof(pct_str), "%.1f* (%.1f)", subjects[i].simulated_pct, subjects[i].percentage);
        } else {
            snprintf(pct_str, sizeof(pct_str), "%.2f%%", subjects[i].percentage);
        }

        printf("%-3d | %-32s | %7d | %5d | %10s | %7d | %6d | %-15s | %10.2f%%\n",
               i + 1,
               name_buf,
               subjects[i].present,
               subjects[i].held,
               pct_str,
               subjects[i].pending_classes,
               subjects[i].future_classes,
               target_buf,
               subjects[i].max_achievable);
    }
    printf("===============================================================================================================\n");
    printf("Total Subjects: %d (* indicates simulated %% with temporary adjustments)\n\n", count);
}

static void handle_what_if(struct Subject *subjects, int count) {
    char input[64];
    printf("Select subject number (1-%d): ", count);
    read_input(input, sizeof(input), 0);
    int idx = atoi(input) - 1;

    if (idx < 0 || idx >= count) {
        printf("Invalid subject number.\n");
        return;
    }

    struct Subject *s = &subjects[idx];
    printf("\n--- What-If Calculator: %s ---\n", s->name);
    printf("Current: %d attended / %d held (Condoned: %d) -> %.2f%%\n",
           s->present, s->held, s->condoned, s->percentage);
    printf("Scheduled Future Classes: %d\n\n", s->future_classes);

    printf("Enter number of future classes you will ATTEND: ");
    read_input(input, sizeof(input), 0);
    int will_attend = atoi(input);

    printf("Enter number of future classes you will MISS: ");
    read_input(input, sizeof(input), 0);
    int will_miss = atoi(input);

    if (will_attend < 0 || will_miss < 0) {
        printf("Counts cannot be negative.\n");
        return;
    }

    int new_p = s->present + will_attend;
    int new_held = s->held + will_attend + will_miss;
    int new_denom = (s->held - s->condoned) + will_attend + will_miss;
    double new_pct = new_denom > 0 ? ((double)new_p * 100.0) / new_denom : 0.0;

    printf("\nProjection Result:\n");
    printf("  Projected Present: %d\n", new_p);
    printf("  Projected Held   : %d\n", new_held);
    printf("  Projected %%      : %.2f%% (%s)\n\n",
           new_pct, new_pct >= 75.0 ? "Eligible" : "Attendance Shortage");
}

static void handle_pending_adjustment(struct Subject *subjects, int count) {
    struct {
        int subj_idx;
        int item_idx;
    } flat_pending[64];
    int total_pending = 0;

    printf("\n--- Pending Classes (Last 4 Days) ---\n");
    for (int i = 0; i < count; i++) {
        for (int k = 0; k < subjects[i].pending_classes; k++) {
            if (total_pending < 64) {
                flat_pending[total_pending].subj_idx = i;
                flat_pending[total_pending].item_idx = k;

                const char *choice_label = "[Unset]";
                if (subjects[i].pending_items[k].user_choice == 1) choice_label = "[YES - Attended]";
                else if (subjects[i].pending_items[k].user_choice == 2) choice_label = "[NO - Missed]";

                printf("  %d. %-30s | Date: %s | Time: %-13s | %s\n",
                       total_pending + 1,
                       subjects[i].name,
                       subjects[i].pending_items[k].date_str,
                       subjects[i].pending_items[k].time_str,
                       choice_label);

                total_pending++;
            }
        }
    }

    if (total_pending == 0) {
        printf("  No pending classes found from the last 4 days.\n\n");
        return;
    }

    char input[64];
    printf("\nSelect class number to adjust (1-%d, or 0 to cancel): ", total_pending);
    read_input(input, sizeof(input), 0);
    int sel = atoi(input) - 1;

    if (sel < 0 || sel >= total_pending) {
        return;
    }

    int si = flat_pending[sel].subj_idx;
    int pi = flat_pending[sel].item_idx;
    struct Subject *s = &subjects[si];

    printf("\nClass: %s (%s, %s)\n", s->name, s->pending_items[pi].date_str, s->pending_items[pi].time_str);
    printf("1. Mark YES (Attended)\n");
    printf("2. Mark NO (Missed)\n");
    printf("3. Reset (Unset)\n");
    printf("Choice: ");
    read_input(input, sizeof(input), 0);

    if (strcmp(input, "1") == 0) {
        s->pending_items[pi].user_choice = 1;
        printf("Marked as Attended (YES).\n");
    } else if (strcmp(input, "2") == 0) {
        s->pending_items[pi].user_choice = 2;
        printf("Marked as Missed (NO).\n");
    } else if (strcmp(input, "3") == 0) {
        s->pending_items[pi].user_choice = 0;
        printf("Temporary selection cleared.\n");
    } else {
        printf("Invalid choice.\n");
        return;
    }

    calculate_metrics(s);
    printf("Updated Simulated Attendance for %s: %.2f%% (Official: %.2f%%)\n\n",
           s->name, s->simulated_pct, s->percentage);
}

int main(void) {
    char username[128] = {0};
    char password[128] = {0};
    char token[TOKEN_SIZE] = {0};
    char student_id[ID_SIZE] = {0};
    struct Subject subjects[MAX_SUBJECTS];
    int count = 0;

    printf("==============================================\n");
    printf("   AttendIQ — Student Attendance Analyser    \n");
    printf("==============================================\n\n");

    printf("Enter Student ID: ");
    fflush(stdout);
    read_input(username, sizeof(username), 0);

    if (strlen(username) == 0) {
        printf("Error: Student ID cannot be empty.\n");
        return 1;
    }

    printf("Enter Password: ");
    fflush(stdout);
    read_input(password, sizeof(password), 1);

    if (strlen(password) == 0) {
        printf("Error: Password cannot be empty.\n");
        return 1;
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);
    CURL *curl = curl_easy_init();
    if (!curl) {
        printf("Error: Failed to initialize network connection.\n");
        memset(password, 0, sizeof(password));
        curl_global_cleanup();
        return 1;
    }

    printf("\nConnecting to MyUPES...\n");
    if (!authenticate(curl, username, password, token, student_id)) {
        printf("Login failed. Check your student ID and password.\n");
        memset(password, 0, sizeof(password));
        curl_easy_cleanup(curl);
        curl_global_cleanup();
        return 1;
    }

    memset(password, 0, sizeof(password));

    printf("Fetching live attendance and timetable...\n");
    if (!fetch_attendance(curl, token, student_id, subjects, &count)) {
        printf("Failed to retrieve attendance data.\n");
        curl_easy_cleanup(curl);
        curl_global_cleanup();
        return 1;
    }

    fetch_timetable(curl, token, student_id, subjects, count);

    print_attendance_table(subjects, count);

    char choice[32];
    while (1) {
        printf("--- AttendIQ Menu ---\n");
        printf("1. View Subject Details & What-If Calculator\n");
        printf("2. Adjust Pending Attendance (Temporary Yes/No)\n");
        printf("3. Refresh Live MyUPES Data\n");
        printf("4. Display Attendance Summary Table\n");
        printf("5. Exit\n");
        printf("Enter Choice (1-5): ");
        read_input(choice, sizeof(choice), 0);

        if (strcmp(choice, "1") == 0) {
            handle_what_if(subjects, count);
        } else if (strcmp(choice, "2") == 0) {
            handle_pending_adjustment(subjects, count);
        } else if (strcmp(choice, "3") == 0) {
            printf("\nRefreshing live attendance and timetable data...\n");
            if (fetch_attendance(curl, token, student_id, subjects, &count)) {
                fetch_timetable(curl, token, student_id, subjects, count);
                printf("Live data refreshed successfully. Temporary adjustments cleared.\n");
                print_attendance_table(subjects, count);
            } else {
                printf("Failed to refresh attendance data.\n");
            }
        } else if (strcmp(choice, "4") == 0) {
            print_attendance_table(subjects, count);
        } else if (strcmp(choice, "5") == 0) {
            printf("Exiting AttendIQ. Goodbye!\n");
            break;
        } else {
            printf("Invalid choice. Please select 1-5.\n\n");
        }
    }

    curl_easy_cleanup(curl);
    curl_global_cleanup();
    return 0;
}

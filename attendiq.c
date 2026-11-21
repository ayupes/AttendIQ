#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <math.h>
#include <curl/curl.h>
#include "cJSON.h"
#include "attendiq.h"

#define SSO_BASE "https://myupes-beta.upes.ac.in/sso"
#define API_BASE "https://myupes-beta.upes.ac.in/apigateway"
#define CLIENT_ID 3
#define CLIENT_SECRET "ku7GUMtyT8er51rTfTc7HC"
#define REFERER "https://myupes-beta.upes.ac.in/connectportal/app/auth/login"
#define USER_AGENT "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36"

struct Buffer {
    char *data;
    size_t size;
};

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userp) {
    size_t total = size * nmemb;
    struct Buffer *buf = (struct Buffer *)userp;
    char *p = realloc(buf->data, buf->size + total + 1);
    if (!p) return 0;
    buf->data = p;
    memcpy(&(buf->data[buf->size]), ptr, total);
    buf->size += total;
    buf->data[buf->size] = '\0';
    return total;
}

static cJSON *send_request(CURL *curl, const char *url, const char *post_data,
                           const char *token, const char *student_id) {
    struct Buffer buf = {malloc(1), 0};
    if (!buf.data) return NULL;
    buf.data[0] = '\0';

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json;charset=UTF-8");
    headers = curl_slist_append(headers, "Accept: application/json, text/plain, */*");
    headers = curl_slist_append(headers, "Origin: https://myupes-beta.upes.ac.in");
    headers = curl_slist_append(headers, "User-Agent: " USER_AGENT);
    headers = curl_slist_append(headers, "x-applicationname: connectportal");
    headers = curl_slist_append(headers, "x-requestfrom: web");
    headers = curl_slist_append(headers, "x-appsecret: " CLIENT_SECRET);
    headers = curl_slist_append(headers, "Referer: " REFERER);

    if (student_id && strlen(student_id) > 0) {
        char id_hdr[256];
        snprintf(id_hdr, sizeof(id_hdr), "x-studentUniqueId: %s", student_id);
        headers = curl_slist_append(headers, id_hdr);
    }

    if (token && strlen(token) > 0) {
        char auth_hdr[TOKEN_SIZE + 32];
        snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s", token);
        headers = curl_slist_append(headers, auth_hdr);
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&buf);
    curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);

    if (post_data) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data);
    } else {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    }

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);

    if (res != CURLE_OK || http_code < 200 || http_code >= 300) {
        free(buf.data);
        return NULL;
    }

    cJSON *json = cJSON_Parse(buf.data);
    free(buf.data);
    return json;
}

static int get_code_from_url(CURL *curl, const char *url, char *code, size_t max_len) {
    const char *start = strstr(url, "code=");
    if (!start) return 0;
    start += 5;

    const char *end = start;
    while (*end && *end != '&' && *end != ' ' && *end != '"') end++;

    size_t len = end - start;
    char temp[512];
    if (len >= sizeof(temp)) return 0;
    strncpy(temp, start, len);
    temp[len] = '\0';

    int dec_len = 0;
    char *decoded = curl_easy_unescape(curl, temp, (int)len, &dec_len);
    if (decoded) {
        strncpy(code, decoded, max_len - 1);
        code[max_len - 1] = '\0';
        curl_free(decoded);
        return 1;
    }
    return 0;
}

static const char *extract_jwt(cJSON *node) {
    if (!node) return NULL;
    if (cJSON_IsString(node)) {
        if (strncmp(node->valuestring, "ey", 2) == 0 && strchr(node->valuestring, '.')) {
            return node->valuestring;
        }
    }
    for (cJSON *child = node->child; child != NULL; child = child->next) {
        const char *found = extract_jwt(child);
        if (found) return found;
    }
    return NULL;
}

static int is_valid_guid(const char *s) {
    if (!s || strlen(s) != 36) return 0;
    if (s[8] != '-' || s[13] != '-' || s[18] != '-' || s[23] != '-') return 0;
    for (int i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) continue;
        if (!isxdigit((unsigned char)s[i])) return 0;
    }
    return 1;
}

static const char *find_guid_in_json(cJSON *node) {
    if (!node) return NULL;
    if (cJSON_IsObject(node) || cJSON_IsArray(node)) {
        for (cJSON *child = node->child; child != NULL; child = child->next) {
            if (child->string && (strstr(child->string, "StudentId") ||
                                  strstr(child->string, "StudentUniqueID") ||
                                  strstr(child->string, "StudentUniqueId") ||
                                  strstr(child->string, "GlobalId"))) {
                if (cJSON_IsString(child) && is_valid_guid(child->valuestring)) {
                    return child->valuestring;
                }
            }
            const char *res = find_guid_in_json(child);
            if (res) return res;
        }
    }
    return NULL;
}

static int is_course_item(cJSON *item) {
    if (!item || !cJSON_IsObject(item)) return 0;
    return (cJSON_GetObjectItem(item, "CourseName") ||
            cJSON_GetObjectItem(item, "ModuleName") ||
            cJSON_GetObjectItem(item, "AttendanceDetails")) ? 1 : 0;
}

static cJSON *find_course_array_recursive(cJSON *node) {
    if (!node) return NULL;
    if (cJSON_IsArray(node) && cJSON_GetArraySize(node) > 0) {
        if (is_course_item(cJSON_GetArrayItem(node, 0))) return node;
    }
    for (cJSON *child = node->child; child != NULL; child = child->next) {
        if (cJSON_IsObject(child) || cJSON_IsArray(child)) {
            cJSON *res = find_course_array_recursive(child);
            if (res) return res;
        }
    }
    return NULL;
}

static cJSON *find_course_array(cJSON *root) {
    if (!root) return NULL;
    cJSON *item = cJSON_GetObjectItem(root, "Item");
    if (!item) item = cJSON_GetObjectItem(root, "data");
    if (!item) item = root;

    const char *keys[] = {"CourseAttendanceInfo", "AttendanceInfo", "CourseList", NULL};
    for (int i = 0; keys[i]; i++) {
        cJSON *arr = cJSON_GetObjectItem(item, keys[i]);
        if (arr && cJSON_IsArray(arr) && cJSON_GetArraySize(arr) > 0) return arr;
    }
    return find_course_array_recursive(root);
}

static int get_month_num(const char *mon) {
    const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                            "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    for (int i = 0; i < 12; i++) {
        if (strncasecmp(mon, months[i], 3) == 0) return i + 1;
    }
    return 0;
}

static int parse_date_to_int(const char *s, int target_year) {
    if (!s) return -1;
    while (*s && isspace((unsigned char)*s)) s++;
    if (strlen(s) < 8) return -1;

    char buf[64];
    strncpy(buf, s, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *t_ptr = strchr(buf, 'T');
    if (t_ptr) *t_ptr = '\0';
    t_ptr = strchr(buf, ' ');
    if (t_ptr && (t_ptr - buf >= 8)) *t_ptr = '\0';

    char *saveptr = NULL;
    char *t1 = strtok_r(buf, "-/ ", &saveptr);
    char *t2 = strtok_r(NULL, "-/ ", &saveptr);
    char *t3 = strtok_r(NULL, "-/ ", &saveptr);
    if (!t1 || !t2 || !t3) return -1;

    int y = 0, m = 0, d = 0;
    if (strlen(t1) == 4 && isdigit((unsigned char)t1[0])) {
        y = atoi(t1);
        m = isalpha((unsigned char)t2[0]) ? get_month_num(t2) : atoi(t2);
        d = atoi(t3);
    } else if (strlen(t3) >= 4 && isdigit((unsigned char)t3[0])) {
        y = atoi(t3);
        d = atoi(t1);
        m = isalpha((unsigned char)t2[0]) ? get_month_num(t2) : atoi(t2);
    } else {
        return -1;
    }

    if (y <= 0 || m <= 0 || m > 12 || d <= 0 || d > 31) return -1;

    if (target_year > 0 && y != target_year) {
        y = (m >= 6) ? target_year : target_year + 1;
    }
    return y * 10000 + m * 100 + d;
}

static int parse_time_to_minutes(const char *s) {
    if (!s || strlen(s) == 0) return -1;
    while (*s && isspace((unsigned char)*s)) s++;

    int h = 0, m = 0;
    if (sscanf(s, "%d:%d", &h, &m) < 2) return -1;

    if (strcasestr(s, "PM") || strcasestr(s, "P.M.")) {
        if (h < 12) h += 12;
    } else if (strcasestr(s, "AM") || strcasestr(s, "A.M.")) {
        if (h == 12) h = 0;
    }
    return h * 60 + m;
}

static int matches_subject(const char *s_name, const char *s_code,
                           const char *a_name, const char *a_code) {
    if (!a_name || !s_name) return 0;

    // Exact course code match
    if (s_code && a_code && strlen(s_code) >= 3 && strcasecmp(s_code, a_code) == 0) return 1;

    // Exact or substring course name match
    if (strcasecmp(s_name, a_name) == 0) return 1;
    if (strlen(s_name) >= 6 && strlen(a_name) >= 6 &&
        (strcasestr(s_name, a_name) || strcasestr(a_name, s_name))) return 1;

    // Multi-word domain keywords
    const char *keywords[] = {"Environment", "EVS", "Machine Learning", "Cloud",
                              "Network", "Operating", "Design", "Data Science", "Security", NULL};
    for (int i = 0; keywords[i]; i++) {
        if (strcasestr(s_name, keywords[i]) && strcasestr(a_name, keywords[i])) return 1;
    }

    if ((strcasestr(s_name, "Programming in C") || (strcasestr(s_name, "Programming") && strcasestr(s_name, " C"))) &&
        (strcasestr(a_name, "Programming in C") || (strcasestr(a_name, "Programming") && strcasestr(a_name, " C")))) {
        return 1;
    }
    if (strcasestr(s_name, "Problem Solving") && strcasestr(a_name, "Problem Solving")) return 1;

    return 0;
}

void calculate_metrics(struct Subject *s) {
    int p = s->present;
    int h = s->held;
    int c = s->condoned;
    int denom = h - c;

    // Official percentage
    s->percentage = (denom > 0) ? ((double)p * 100.0) / denom : 0.0;

    // Classes needed to reach 75%
    if (denom > 0 && s->percentage < 75.0) {
        double needed = (0.75 * (double)denom - (double)p) / 0.25;
        s->needed_for_75 = (int)ceil(needed);
        if (s->needed_for_75 < 0) s->needed_for_75 = 0;
    } else {
        s->needed_for_75 = 0;
    }

    // Classes safely skippable while maintaining 75%
    if (denom > 0 && s->percentage >= 75.0) {
        int max_held = (int)floor((double)p / 0.75);
        int max_skip = max_held - denom;
        if (max_skip < 0) max_skip = 0;
        s->safe_to_skip = (max_skip > s->future_classes) ? s->future_classes : max_skip;
    } else {
        s->safe_to_skip = 0;
    }

    // Maximum achievable final percentage
    int total_remaining = s->pending_classes + s->future_classes;
    int final_denom = denom + total_remaining;
    if (final_denom > 0) {
        s->max_achievable = ((double)(p + total_remaining) * 100.0) / final_denom;
        if (s->max_achievable > 100.0) s->max_achievable = 100.0;
    } else {
        s->max_achievable = s->percentage;
    }

    // Simulated percentage with temporary pending adjustments
    int yes_count = 0, no_count = 0;
    for (int k = 0; k < s->pending_classes; k++) {
        if (s->pending_items[k].user_choice == 1) yes_count++;
        else if (s->pending_items[k].user_choice == 2) no_count++;
    }

    int sim_p = p + yes_count;
    int sim_denom = denom + yes_count + no_count;
    s->simulated_pct = (sim_denom > 0) ? ((double)sim_p * 100.0) / sim_denom : s->percentage;
}

int authenticate(CURL *curl, const char *username, const char *password,
                 char *access_token, char *student_id) {
    cJSON *login_req = cJSON_CreateObject();
    cJSON_AddStringToObject(login_req, "UserId", username);
    cJSON_AddStringToObject(login_req, "Password", password);
    cJSON_AddStringToObject(login_req, "Captcha", "-");
    cJSON_AddNullToObject(login_req, "AppRef");
    cJSON_AddBoolToObject(login_req, "RememberMe", 0);

    char *login_payload = cJSON_PrintUnformatted(login_req);
    cJSON_Delete(login_req);

    cJSON *login_res = send_request(curl, SSO_BASE "/api/account/oauth2/token",
                                    login_payload, NULL, NULL);
    free(login_payload);
    if (!login_res) return 0;

    cJSON *item = cJSON_GetObjectItem(login_res, "Item");
    cJSON *redirect = item ? cJSON_GetObjectItem(item, "redirectUrl") : NULL;
    char code[1024] = {0};

    if (!redirect || !cJSON_IsString(redirect) ||
        !get_code_from_url(curl, redirect->valuestring, code, sizeof(code))) {
        cJSON_Delete(login_res);
        return 0;
    }
    cJSON_Delete(login_res);

    cJSON *token_req = cJSON_CreateObject();
    cJSON_AddNumberToObject(token_req, "ClientId", CLIENT_ID);
    cJSON_AddStringToObject(token_req, "ClientSecret", CLIENT_SECRET);
    cJSON_AddStringToObject(token_req, "Code", code);

    char *token_payload = cJSON_PrintUnformatted(token_req);
    cJSON_Delete(token_req);

    cJSON *token_res = send_request(curl, SSO_BASE "/oauth2/access_token",
                                    token_payload, NULL, NULL);
    free(token_payload);
    if (!token_res) return 0;

    const char *jwt = extract_jwt(token_res);
    if (!jwt) {
        cJSON_Delete(token_res);
        return 0;
    }
    strncpy(access_token, jwt, TOKEN_SIZE - 1);
    access_token[TOKEN_SIZE - 1] = '\0';

    char fallback_guid[64] = {0};
    const char *t_guid = find_guid_in_json(token_res);
    if (t_guid) strncpy(fallback_guid, t_guid, sizeof(fallback_guid) - 1);
    cJSON_Delete(token_res);

    cJSON *info_res = send_request(curl, API_BASE "/connect-portal/api/studentloginbasicinfo",
                                   NULL, access_token, NULL);
    if (!info_res) return 0;

    const char *guid = find_guid_in_json(info_res);
    if (!guid && strlen(fallback_guid) > 0) guid = fallback_guid;

    if (!guid) {
        cJSON_Delete(info_res);
        return 0;
    }

    strncpy(student_id, guid, ID_SIZE - 1);
    student_id[ID_SIZE - 1] = '\0';
    cJSON_Delete(info_res);
    return 1;
}

int fetch_attendance(CURL *curl, const char *access_token, const char *student_id,
                     struct Subject *subjects, int *subject_count) {
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    int target_year = tm_now.tm_year + 1900;

    char payload[512];
    snprintf(payload, sizeof(payload), "{\"StudentUniqueID\":\"%s\"}", student_id);

    cJSON *res = send_request(curl, API_BASE "/student-attendance/studentattendancesummary",
                              payload, access_token, student_id);
    if (!res) return 0;

    cJSON *courses = find_course_array(res);
    int total = (courses && cJSON_IsArray(courses)) ? cJSON_GetArraySize(courses) : 0;
    if (total == 0) {
        cJSON_Delete(res);
        return 0;
    }

    if (total > MAX_SUBJECTS) total = MAX_SUBJECTS;
    *subject_count = total;

    for (int i = 0; i < total; i++) {
        cJSON *item = cJSON_GetArrayItem(courses, i);
        memset(&subjects[i], 0, sizeof(struct Subject));

        cJSON *name = cJSON_GetObjectItem(item, "CourseName");
        if (!name) name = cJSON_GetObjectItem(item, "ModuleName");
        cJSON *code = cJSON_GetObjectItem(item, "CourseCode");
        if (!code) code = cJSON_GetObjectItem(item, "ModuleCode");

        if (name && name->valuestring) strncpy(subjects[i].name, name->valuestring, sizeof(subjects[i].name) - 1);
        if (code && code->valuestring) strncpy(subjects[i].code, code->valuestring, sizeof(subjects[i].code) - 1);

        cJSON *details = cJSON_GetObjectItem(item, "AttendanceDetails");
        if (details && cJSON_IsArray(details)) {
            int sessions = cJSON_GetArraySize(details);
            subjects[i].held = sessions;

            for (int s = 0; s < sessions; s++) {
                cJSON *sess = cJSON_GetArrayItem(details, s);
                cJSON *status = cJSON_GetObjectItem(sess, "AttendanceStatus");
                if (!status) status = cJSON_GetObjectItem(sess, "Status");
                cJSON *subtype = cJSON_GetObjectItem(sess, "AttendanceSubTypeCode");

                const char *st = status && status->valuestring ? status->valuestring : "";
                const char *sub = subtype && subtype->valuestring ? subtype->valuestring : "";

                if (strcasecmp(st, "PRESENT") == 0) subjects[i].present++;
                else if (strstr(sub, "CNDN") || strcasecmp(st, "CONDONED") == 0) subjects[i].condoned++;
                else subjects[i].absent++;

                cJSON *sdate = cJSON_GetObjectItem(sess, "SessionDate");
                cJSON *stime = cJSON_GetObjectItem(sess, "SessionTime");
                if (sdate && sdate->valuestring && subjects[i].session_count < MAX_SESSIONS) {
                    int sd = parse_date_to_int(sdate->valuestring, target_year);
                    if (sd > 0) {
                        int idx = subjects[i].session_count++;
                        subjects[i].sessions[idx].date_int = sd;
                        subjects[i].sessions[idx].matched = 0;
                        strncpy(subjects[i].sessions[idx].status, st, 15);

                        const char *t_str = stime && stime->valuestring ? stime->valuestring : "";
                        strncpy(subjects[i].sessions[idx].time_str, t_str, 31);
                        subjects[i].sessions[idx].start_min = parse_time_to_minutes(t_str);
                    }
                }
            }
        }

        if (subjects[i].held == 0) {
            cJSON *pres = cJSON_GetObjectItem(item, "TotalPresent");
            cJSON *held = cJSON_GetObjectItem(item, "TotalHeld");
            if (pres && cJSON_IsNumber(pres)) subjects[i].present = pres->valueint;
            if (held && cJSON_IsNumber(held)) subjects[i].held = held->valueint;
            subjects[i].absent = subjects[i].held - subjects[i].present;
        }

        calculate_metrics(&subjects[i]);
    }

    cJSON_Delete(res);
    return 1;
}

int fetch_timetable(CURL *curl, const char *access_token, const char *student_id,
                    struct Subject *subjects, int subject_count) {
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    int target_year = tm_now.tm_year + 1900;
    int today_int = target_year * 10000 + (tm_now.tm_mon + 1) * 100 + tm_now.tm_mday;
    int now_mins = tm_now.tm_hour * 60 + tm_now.tm_min;

    // 4-day pending window
    time_t four_days_ago = now - (4 * 86400);
    struct tm tm_4d;
    localtime_r(&four_days_ago, &tm_4d);
    int min_pending_date = (tm_4d.tm_year + 1900) * 10000 + (tm_4d.tm_mon + 1) * 100 + tm_4d.tm_mday;

    for (int i = 0; i < subject_count; i++) {
        subjects[i].future_classes = 0;
        subjects[i].pending_classes = 0;
        subjects[i].seen_slot_count = 0;
        for (int k = 0; k < subjects[i].session_count; k++) {
            subjects[i].sessions[k].matched = 0;
        }
    }

    char payload[512];
    snprintf(payload, sizeof(payload),
             "{\"ActivityCode\":\"student\",\"TimeTableContextDetails\":{\"StudentCode\":\"%s\"}}",
             student_id);

    cJSON *res = send_request(curl, API_BASE "/api/timetable", payload, access_token, student_id);
    if (!res) {
        snprintf(payload, sizeof(payload),
                 "{\"ActivityCode\":\"student\",\"TimeTableContextDetails\":{\"StudentUniqueId\":\"%s\"}}",
                 student_id);
        res = send_request(curl, API_BASE "/api/timetable", payload, access_token, student_id);
    }
    if (!res) return 0;

    cJSON *slots = cJSON_IsArray(res) ? res : cJSON_GetObjectItem(res, "Item");
    if (!slots) slots = cJSON_GetObjectItem(res, "data");
    if (!slots || !cJSON_IsArray(slots)) {
        cJSON_Delete(res);
        return 0;
    }

    int total_slots = cJSON_GetArraySize(slots);
    for (int t = 0; t < total_slots; t++) {
        cJSON *slot = cJSON_GetArrayItem(slots, t);
        if (!slot) continue;

        cJSON *sdate = cJSON_GetObjectItem(slot, "SlotDate");
        if (!sdate) sdate = cJSON_GetObjectItem(slot, "Date");
        cJSON *stime = cJSON_GetObjectItem(slot, "SlotStartTime");
        cJSON *etime = cJSON_GetObjectItem(slot, "SlotEndTime");

        const char *raw_date = sdate && sdate->valuestring ? sdate->valuestring : "";
        int slot_date = parse_date_to_int(raw_date, target_year);
        if (slot_date <= 0) continue;

        const char *stime_str = stime && stime->valuestring ? stime->valuestring : "";
        const char *etime_str = etime && etime->valuestring ? etime->valuestring : "";

        int slot_start = parse_time_to_minutes(stime_str);
        int slot_end = parse_time_to_minutes(etime_str);

        int has_ended = 0;
        if (slot_date < today_int) {
            has_ended = 1;
        } else if (slot_date == today_int && slot_end > 0 && slot_end <= now_mins) {
            has_ended = 1;
        }

        char slot_name[128] = {0};
        char slot_code[64] = {0};
        cJSON *mod = slot;
        cJSON *mlist = cJSON_GetObjectItem(slot, "ModuleList");
        if (mlist && cJSON_IsArray(mlist) && cJSON_GetArraySize(mlist) > 0) {
            mod = cJSON_GetArrayItem(mlist, 0);
        }
        cJSON *mn = cJSON_GetObjectItem(mod, "ModuleName");
        if (!mn) mn = cJSON_GetObjectItem(mod, "CourseName");
        cJSON *mc = cJSON_GetObjectItem(mod, "ModuleCode");
        if (!mc) mc = cJSON_GetObjectItem(mod, "CourseCode");
        if (mn && mn->valuestring) strncpy(slot_name, mn->valuestring, sizeof(slot_name) - 1);
        if (mc && mc->valuestring) strncpy(slot_code, mc->valuestring, sizeof(slot_code) - 1);

        for (int i = 0; i < subject_count; i++) {
            if (matches_subject(slot_name, slot_code, subjects[i].name, subjects[i].code)) {
                // Deduplicate identical slot
                int is_dup = 0;
                for (int d = 0; d < subjects[i].seen_slot_count; d++) {
                    if (subjects[i].seen_slots[d].date_int == slot_date &&
                        strcmp(subjects[i].seen_slots[d].start_time, stime_str) == 0) {
                        is_dup = 1;
                        break;
                    }
                }
                if (is_dup) break;

                if (subjects[i].seen_slot_count < MAX_SESSIONS) {
                    subjects[i].seen_slots[subjects[i].seen_slot_count].date_int = slot_date;
                    strncpy(subjects[i].seen_slots[subjects[i].seen_slot_count].start_time, stime_str, 15);
                    subjects[i].seen_slot_count++;
                }

                if (!has_ended) {
                    subjects[i].future_classes++;
                } else {
                    int matched_idx = -1;
                    // Pass 1: exact date + start time within 30 mins
                    if (slot_start > 0) {
                        for (int k = 0; k < subjects[i].session_count; k++) {
                            if (!subjects[i].sessions[k].matched &&
                                subjects[i].sessions[k].date_int == slot_date &&
                                subjects[i].sessions[k].start_min > 0 &&
                                abs(subjects[i].sessions[k].start_min - slot_start) <= 30) {
                                matched_idx = k;
                                break;
                            }
                        }
                    }
                    // Pass 2: same date fallback for shifted sessions
                    if (matched_idx < 0) {
                        for (int k = 0; k < subjects[i].session_count; k++) {
                            if (!subjects[i].sessions[k].matched &&
                                subjects[i].sessions[k].date_int == slot_date) {
                                matched_idx = k;
                                break;
                            }
                        }
                    }

                    if (matched_idx >= 0) {
                        subjects[i].sessions[matched_idx].matched = 1;
                    } else if (slot_date >= min_pending_date) {
                        if (subjects[i].pending_classes < 16) {
                            int p_idx = subjects[i].pending_classes++;
                            subjects[i].pending_items[p_idx].date_int = slot_date;
                            snprintf(subjects[i].pending_items[p_idx].date_str,
                                     sizeof(subjects[i].pending_items[p_idx].date_str),
                                     "%04d-%02d-%02d",
                                     slot_date / 10000,
                                     (slot_date % 10000) / 100,
                                     slot_date % 100);
                            snprintf(subjects[i].pending_items[p_idx].time_str,
                                     sizeof(subjects[i].pending_items[p_idx].time_str),
                                     "%s-%s", stime_str, etime_str);
                            subjects[i].pending_items[p_idx].user_choice = 0;
                        }
                    }
                }
                break;
            }
        }
    }

    for (int i = 0; i < subject_count; i++) {
        calculate_metrics(&subjects[i]);
    }

    cJSON_Delete(res);
    return 1;
}

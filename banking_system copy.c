#include <stdio.h>         // #inport...
#include <stdlib.h>        // #define...
#include <stdint.h>        
#include <stdbool.h>
#include <string.h>        // for typing
#include <ctype.h>         // int/str/... 
#include <time.h>          // import time
#include <errno.h>
#include <limits.h>
#include <stdarg.h>   
#include <float.h>    

/* ========== Config ========== */   
#define DB_DIR             "database"
#define INDEX_PATH         DB_DIR "/index.txt"
#define LOG_PATH           DB_DIR "/transaction.log"
#define ACCT_FILE_FMT      DB_DIR "/%s.txt"    
#define MIN_ACCT_DIGITS    7
#define MAX_ACCT_DIGITS    9
#define MAX_NAME_LEN       80
#define MAX_ID_LEN         40
#define PIN_LEN            4
#define MAX_DEPOSIT_PER_OP 50000.0

/* ========== Types ========== */
typedef enum { ACCT_SAVINGS = 1, ACCT_CURRENT = 2 } AccountType;

typedef struct {
    char account[16];           // up to 9 digits + '\0' 
    char name[MAX_NAME_LEN+1];
    char id[MAX_ID_LEN+1];
    AccountType type;
    char pin[PIN_LEN+1];        // 4 chars + '\0' 
    double balance;
} Account;

/* ========== String helpers ========== */
static void rstrip_newline(char *s) {
    if (!s) return;
    size_t n = strlen(s);
    while (n && (s[n-1] == '\n' || s[n-1] == '\r')) { s[--n] = '\0'; }
}

static void trim(char *s) {
    if (!s) return;
    // left trim
    size_t i = 0, n = strlen(s);
    while (i < n && isspace((unsigned char)s[i])) i++;
    if (i) memmove(s, s+i, n - i + 1);
    // right trim
    rstrip_newline(s);
    n = strlen(s);
    while (n && isspace((unsigned char)s[n-1])) s[--n] = '\0';
}

static void tolower_str(char *s) {
    for (; s && *s; ++s) *s = (char)tolower((unsigned char)*s);
}

/* ========== Safe input parsing ========== */
static void prompt(const char *msg, char *buf, size_t buflen) {
    for (;;) {
        fputs(msg, stdout);
        if (!fgets(buf, (int)buflen, stdin)) { clearerr(stdin); continue; }
        rstrip_newline(buf);
        if (buf[0] == '\0') { puts("Input cannot be empty. Please try again."); continue; }
        return;
    }
}

/* Parse long with bounds and error messages; returns true on success. */
static bool parse_long(const char *s, long *out, long minv, long maxv) {
    errno = 0;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0') return false;
    if (v < minv || v > maxv) return false;
    *out = v;
    return true;
}

/* Parse double > 0 with upper bound; returns true on success. */
static bool parse_double(const char *s, double *out, double minv, double maxv) {
    errno = 0;
    char *end = NULL;
    double v = strtod(s, &end);
    if (errno != 0 || end == s || *end != '\0') return false;
    if (v < minv || v > maxv) return false;
    *out = v;
    return true;
}

/* ========== Files & logging ========== */
static bool ensure_db_present(void) {
    /* We are not creating directories (to keep to core headers).
       Just check if index/log can be opened/created in DB_DIR. */
    FILE *f = fopen(INDEX_PATH, "a+");
    if (!f) return false;
    fclose(f);
    f = fopen(LOG_PATH, "a+");
    if (!f) return false;
    fclose(f);
    return true;
}

static void log_action(const char *fmt, ...) {
    FILE *logf = fopen(LOG_PATH, "a");
    if (!logf) return;
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char ts[32];
    if (tm) strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm);
    else    snprintf(ts, sizeof(ts), "unknown-time");

    fprintf(logf, "[%s] ", ts);
    va_list ap; va_start(ap, fmt);
    vfprintf(logf, fmt, ap);
    va_end(ap);
    fputc('\n', logf);
    fclose(logf);
}

/* Count accounts from index.txt */
static size_t count_accounts(void) {
    FILE *f = fopen(INDEX_PATH, "r");
    if (!f) return 0;
    size_t count = 0;
    char line[64];
    while (fgets(line, sizeof(line), f)) {
        trim(line);
        if (line[0]) count++;
    }
    fclose(f);
    return count;
}

/* Check uniqueness in index.txt */
static bool account_exists_in_index(const char *acct) {
    FILE *f = fopen(INDEX_PATH, "r");
    if (!f) return false;
    char line[64];
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        trim(line);
        if (strcmp(line, acct) == 0) { found = true; break; }
    }
    fclose(f);
    return found;
}

static bool add_to_index(const char *acct) {
    FILE *f = fopen(INDEX_PATH, "a");
    if (!f) return false;
    fprintf(f, "%s\n", acct);
    fclose(f);
    return true;
}

static bool remove_from_index(const char *acct) {
    FILE *in = fopen(INDEX_PATH, "r");
    if (!in) return false;
    char tmpPath[] = DB_DIR "/index.tmp";
    FILE *out = fopen(tmpPath, "w");
    if (!out) { fclose(in); return false; }

    char line[64];
    bool removed = false;
    while (fgets(line, sizeof(line), in)) {
        trim(line);
        if (strcmp(line, acct) == 0) { removed = true; continue; }
        fprintf(out, "%s\n", line);
    }
    fclose(in); fclose(out);
    if (removed) {
        remove(INDEX_PATH);
        rename(tmpPath, INDEX_PATH);
    } else {
        remove(tmpPath);
    }
    return removed;
}

/* Build filename for account */
static void account_filepath(const char *acct, char *out, size_t outlen) {
    snprintf(out, outlen, ACCT_FILE_FMT, acct);
}

/* ========== Account I/O ========== */
static bool write_account_file(const Account *a) {
    char path[256];
    account_filepath(a->account, path, sizeof(path));
    FILE *f = fopen(path, "w");
    if (!f) return false;
    fprintf(f, "account=%s\n", a->account);
    fprintf(f, "name=%s\n", a->name);
    fprintf(f, "id=%s\n", a->id);
    fprintf(f, "type=%s\n", (a->type == ACCT_SAVINGS ? "savings" : "current"));
    fprintf(f, "pin=%s\n", a->pin);
    fprintf(f, "balance=%.2f\n", a->balance);
    fclose(f);
    return true;
}

static bool read_account_file(const char *acct, Account *out) {
    char path[256];
    account_filepath(acct, path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return false;

    memset(out, 0, sizeof(*out));
    strncpy(out->account, acct, sizeof(out->account)-1);

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        trim(line);
        if (!line[0]) continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;

        if (strcmp(key, "name") == 0) {
            strncpy(out->name, val, sizeof(out->name)-1);
        } else if (strcmp(key, "id") == 0) {
            strncpy(out->id, val, sizeof(out->id)-1);
        } else if (strcmp(key, "type") == 0) {
            char tmp[16]; strncpy(tmp, val, sizeof(tmp)-1); tmp[sizeof(tmp)-1]=0;
            tolower_str(tmp);
            out->type = (strcmp(tmp, "current")==0 ? ACCT_CURRENT : ACCT_SAVINGS);
        } else if (strcmp(key, "pin") == 0) {
            strncpy(out->pin, val, sizeof(out->pin)-1);
        } else if (strcmp(key, "balance") == 0) {
            out->balance = atof(val);
        }
    }
    fclose(f);
    return true;
}

static bool delete_account_file(const char *acct) {
    char path[256];
    account_filepath(acct, path, sizeof(path));
    return (remove(path) == 0);
}

/* ========== Account generation & validation ========== */
static void gen_random_account(char *out, size_t outlen) {
    /* Generate 7-9 digit number; ensure first digit 1-9 (no leading zeros) */
    int digits = MIN_ACCT_DIGITS + rand() % (MAX_ACCT_DIGITS - MIN_ACCT_DIGITS + 1);
    char buf[16] = {0};
    buf[0] = '1' + (rand() % 9); /* 1..9 */
    for (int i = 1; i < digits; ++i) buf[i] = '0' + (rand() % 10);
    buf[digits] = '\0';
    strncpy(out, buf, outlen-1);
    out[outlen-1] = '\0';
}

static bool get_unique_account(char *out, size_t outlen) {
    int attempts = 0;
    do {
        gen_random_account(out, outlen);
        if (!account_exists_in_index(out)) return true;
        attempts++;
    } while (attempts < 1000);
    return false;
}

static bool valid_pin(const char *pin) {
    if (strlen(pin) != PIN_LEN) return false;
    for (int i = 0; i < PIN_LEN; ++i)
        if (!isdigit((unsigned char)pin[i])) return false;
    return true;
}

static bool authenticate_pin(const Account *a, const char *pin) {
    return (strncmp(a->pin, pin, PIN_LEN) == 0 && strlen(pin)==PIN_LEN);
}

/* ========== Prompts ==========
   All prompts are looping until valid. */
static void prompt_name(char *out, size_t outlen) {
    for (;;) {
        prompt("Enter full name: ", out, outlen);
        if (strlen(out) > MAX_NAME_LEN) { puts("Name too long."); continue; }
        return;
    }
}

static void prompt_id(char *out, size_t outlen) {
    for (;;) {
        prompt("Enter Identification Number (ID): ", out, outlen);
        if (strlen(out) > MAX_ID_LEN) { puts("ID too long."); continue; }
        return;
    }
}

static AccountType prompt_acct_type(void) {
    char buf[32];
    for (;;) {
        printf("Select account type [1] Savings  [2] Current: ");
        if (!fgets(buf, sizeof(buf), stdin)) { clearerr(stdin); continue; }
        rstrip_newline(buf);
        if (strcmp(buf, "1") == 0) return ACCT_SAVINGS;
        if (strcmp(buf, "2") == 0) return ACCT_CURRENT;
        tolower_str(buf);
        if (strcmp(buf, "s") == 0 || strcmp(buf, "savings") == 0) return ACCT_SAVINGS;
        if (strcmp(buf, "c") == 0 || strcmp(buf, "current") == 0) return ACCT_CURRENT;
        puts("Invalid option. Please choose 1/2 or 'savings'/'current'.");
    }
}

static void prompt_pin(char *out4) {
    char buf[64];
    for (;;) {
        prompt("Set a 4-digit PIN: ", buf, sizeof(buf));
        if (!valid_pin(buf)) { puts("PIN must be exactly 4 digits."); continue; }
        strncpy(out4, buf, PIN_LEN);
        out4[PIN_LEN] = '\0';
        return;
    }
}

static void prompt_existing_acct(char *acct, size_t len) {
    for (;;) {
        prompt("Enter account number: ", acct, len);
        bool ok = true;
        if (strlen(acct) < MIN_ACCT_DIGITS || strlen(acct) > MAX_ACCT_DIGITS) ok = false;
        for (size_t i=0; i<strlen(acct); ++i) if (!isdigit((unsigned char)acct[i])) ok = false;
        if (!ok) { puts("Invalid account number format."); continue; }
        if (!account_exists_in_index(acct)) { puts("Account not found."); continue; }
        return;
    }
}

static void prompt_pin_verify(char pin[PIN_LEN+1]) {
    for (;;) {
        char buf[64];
        prompt("Enter 4-digit PIN: ", buf, sizeof(buf));
        if (!valid_pin(buf)) { puts("Invalid PIN format."); continue; }
        strncpy(pin, buf, PIN_LEN);
        pin[PIN_LEN] = '\0';
        return;
    }
}

static double prompt_amount(double minv, double maxv, const char *label) {
    char buf[128];
    for (;;) {
        printf("Enter amount (RM): ");
        if (!fgets(buf, sizeof(buf), stdin)) { clearerr(stdin); continue; }
        rstrip_newline(buf);
        double v;
        if (!parse_double(buf, &v, minv, maxv)) {
            printf("Invalid %s. Please enter a number between %.2f and %.2f.\n",
                   label, minv, maxv);
            continue;
        }
        return v;
    }
}

/* ========== Operations ========== */
static void op_create(void) {
    Account a;
    memset(&a, 0, sizeof(a));
    if (!get_unique_account(a.account, sizeof(a.account))) {
        puts("Failed to generate a unique account number. Try again.");
        return;
    }
    printf("Generated Account Number: %s\n", a.account);
    prompt_name(a.name, sizeof(a.name));
    prompt_id(a.id, sizeof(a.id));
    a.type = prompt_acct_type();
    prompt_pin(a.pin);
    a.balance = 0.0;

    if (!write_account_file(&a)) {
        puts("Error: Could not write account file. Ensure 'database' folder exists.");
        return;
    }
    if (!add_to_index(a.account)) {
        puts("Warning: Account file written, but failed to update index.");
    }
    printf("Account created successfully. Current balance: RM%.2f\n", a.balance);
    log_action("create account: %s (%s)", a.account, (a.type==ACCT_SAVINGS?"savings":"current"));
}

static void op_delete(void) {
    /* Load current accounts (by index) and let the user choose one. */
    FILE *f = fopen(INDEX_PATH, "r");
    if (!f) { puts("No index found. Nothing to delete."); return; }

    char accts[1024][16];
    size_t n = 0;
    char line[64];
    while (n < 1024 && fgets(line, sizeof(line), f)) {
        trim(line);
        if (line[0]) { strncpy(accts[n], line, sizeof(accts[n])-1); accts[n][15]='\0'; n++; }
    }
    fclose(f);
    if (n == 0) { puts("No accounts to delete."); return; }

    puts("Existing accounts:");
    for (size_t i=0; i<n; ++i) printf("  [%zu] %s\n", i+1, accts[i]);

    /* Choose by number or direct input. */
    char buf[64];
    printf("Select account by number (1-%zu) or type the account number: ", n);
    if (!fgets(buf, sizeof(buf), stdin)) { clearerr(stdin); return; }
    rstrip_newline(buf);

    char acct[16] = {0};
    long idx;
    if (parse_long(buf, &idx, 1, (long)n)) {
        strncpy(acct, accts[idx-1], sizeof(acct)-1);
    } else {
        strncpy(acct, buf, sizeof(acct)-1);
        if (!account_exists_in_index(acct)) { puts("Account not found."); return; }
    }

    Account a;
    if (!read_account_file(acct, &a)) { puts("Failed to load account file."); return; }

    /* Confirm (account number + last 4 of ID + PIN) */
    char pin[PIN_LEN+1];
    char last4[8];
    char idlast4[8];
    prompt("Confirm account number again: ", buf, sizeof(buf));
    if (strcmp(buf, acct) != 0) { puts("Account number mismatch. Abort."); return; }

    size_t idlen = strlen(a.id);
    if (idlen < 4) { puts("ID too short in file; cannot validate."); return; }
    strncpy(idlast4, a.id + (idlen - 4), 5);
    idlast4[4] = '\0';

    prompt("Enter LAST 4 characters of ID to confirm: ", last4, sizeof(last4));
    if (strcmp(last4, idlast4) != 0) { puts("ID confirmation failed. Abort."); return; }

    prompt_pin_verify(pin);
    if (!authenticate_pin(&a, pin)) { puts("PIN incorrect. Abort."); return; }

    if (!delete_account_file(acct)) { puts("Failed to delete account file."); return; }
    if (!remove_from_index(acct))   { puts("Warning: could not remove from index."); }

    printf("Account %s deleted successfully.\n", acct);
    log_action("delete account: %s", acct);
}

static bool load_and_auth(const char *acct, Account *a_out) {
    if (!read_account_file(acct, a_out)) { puts("Account not found or unreadable."); return false; }
    char pin[PIN_LEN+1];
    prompt_pin_verify(pin);
    if (!authenticate_pin(a_out, pin)) { puts("Authentication failed (PIN)."); return false; }
    return true;
}

static void op_deposit(void) {
    char acct[16];
    prompt_existing_acct(acct, sizeof(acct));

    Account a;
    if (!load_and_auth(acct, &a)) return;

    double amt = prompt_amount(0.01, MAX_DEPOSIT_PER_OP, "deposit amount");
    a.balance += amt;

    if (!write_account_file(&a)) { puts("Failed to update account file."); return; }
    printf("Deposit successful. New balance: RM%.2f\n", a.balance);
    log_action("deposit: %s +RM%.2f -> RM%.2f", a.account, amt, a.balance);
}

static void op_withdraw(void) {
    char acct[16];
    prompt_existing_acct(acct, sizeof(acct));

    Account a;
    if (!load_and_auth(acct, &a)) return;

    printf("Available balance: RM%.2f\n", a.balance);
    double amt;
    for (;;) {
        amt = prompt_amount(0.01, DBL_MAX, "withdrawal amount");
        if (amt > a.balance) {
            puts("Insufficient funds. Please try again.");
            continue;
        }
        break;
    }
    a.balance -= amt;

    if (!write_account_file(&a)) { puts("Failed to update account file."); return; }
    printf("Withdrawal successful. New balance: RM%.2f\n", a.balance);
    log_action("withdrawal: %s -RM%.2f -> RM%.2f", a.account, amt, a.balance);
}

static double remittance_fee(AccountType from, AccountType to, double amount) {
    if (from == ACCT_SAVINGS && to == ACCT_CURRENT) return amount * 0.02;
    if (from == ACCT_CURRENT && to == ACCT_SAVINGS) return amount * 0.03;
    return 0.0; /* same type: no fee specified => assume 0% */
}

static void op_remit(void) {
    char src[16], dst[16];
    puts("Sender (From) account:");
    prompt_existing_acct(src, sizeof(src));
    puts("Recipient (To) account:");
    prompt_existing_acct(dst, sizeof(dst));

    if (strcmp(src, dst) == 0) { puts("Sender and recipient must be different."); return; }

    Account A, B;
    if (!load_and_auth(src, &A)) return;
    if (!read_account_file(dst, &B)) { puts("Recipient account unreadable."); return; }

    double amt = prompt_amount(0.01, DBL_MAX, "remittance amount");
    double fee = remittance_fee(A.type, B.type, amt);
    double total = amt + fee;
    if (total > A.balance) { puts("Insufficient funds (including fee)."); return; }

    A.balance -= total;
    B.balance += amt;

    if (!write_account_file(&A) || !write_account_file(&B)) {
        puts("Failed to update accounts. (No change committed)");
        /* In a real system you’d need transactional IO; here we keep it simple. */
        return;
    }
    printf("Remittance successful.\n  Sent: RM%.2f  Fee: RM%.2f\n", amt, fee);
    printf("  Sender new balance: RM%.2f\n  Recipient new balance: RM%.2f\n", A.balance, B.balance);
    log_action("remittance: %s -> %s amount=RM%.2f fee=RM%.2f", A.account, B.account, amt, fee);
}

/* ========== Session & Menu ========== */
static void show_session_banner(void) {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char ts[64];
    if (tm) strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm);
    else    snprintf(ts, sizeof(ts), "unknown-time");
    size_t cnt = count_accounts();
    printf("=== Banking System (Session: %s) ===\n", ts);
    printf("Loaded accounts: %zu\n", cnt);
    puts("-------------------------------------");
}

static void show_menu(void) {
    puts("\nMenu:");
    puts("  1) Create New Bank Account      (keywords: create, new)");
    puts("  2) Delete Bank Account          (keywords: delete, remove)");
    puts("  3) Deposit                      (keywords: deposit)");
    puts("  4) Withdrawal                   (keywords: withdraw, withdrawal)");
    puts("  5) Remittance                   (keywords: remit, transfer)");
    puts("  6) Exit                         (keywords: exit, quit)");
}

static int normalize_choice(const char *in) {
    if (strcmp(in, "1")==0) return 1;
    if (strcmp(in, "2")==0) return 2;
    if (strcmp(in, "3")==0) return 3;
    if (strcmp(in, "4")==0) return 4;
    if (strcmp(in, "5")==0) return 5;
    if (strcmp(in, "6")==0) return 6;

    char s[64]; strncpy(s, in, sizeof(s)-1); s[sizeof(s)-1]=0; tolower_str(s);
    if (strcmp(s, "create")==0 || strcmp(s, "new")==0) return 1;
    if (strcmp(s, "delete")==0 || strcmp(s, "remove")==0) return 2;
    if (strcmp(s, "deposit")==0) return 3;
    if (strcmp(s, "withdraw")==0 || strcmp(s, "withdrawal")==0) return 4;
    if (strcmp(s, "remit")==0 || strcmp(s, "transfer")==0 || strcmp(s, "remittance")==0) return 5;
    if (strcmp(s, "exit")==0 || strcmp(s, "quit")==0) return 6;
    return 0;
}

int main(void) {
    srand((unsigned)time(NULL));

    if (!ensure_db_present()) {
        puts("Error: Could not access 'database' folder.\n"
             "Please create a folder named 'database' next to the executable and run again.");
        return 1;
    }

    show_session_banner();

    char buf[64];
    for (;;) {
        show_menu();
        printf("\nSelect an option (number or keyword): ");
        if (!fgets(buf, sizeof(buf), stdin)) { clearerr(stdin); continue; }
        rstrip_newline(buf);
        int ch = normalize_choice(buf);
        switch (ch) {
            case 1: op_create();    break;
            case 2: op_delete();    break;
            case 3: op_deposit();   break;
            case 4: op_withdraw();  break;
            case 5: op_remit();     break;
            case 6:
                puts("Goodbye!");
                log_action("exit session");
                return 0;
            default:
                puts("Invalid option. Please select a valid menu option.");
                break;
        }
    }
    return 0;
}

/*
 * Attendance vs Academic Performance Analyser 
 * ---------------------------------------------------------
 * Reads a CSV file with columns:  id,attendance,midsem,quiz
 * You choose which marks to analyse: Mid-sem, Quiz, or BOTH.
 * Every statistic and every graph follows that choice.
 *
 * Statistics : mean/median/sd, Pearson, Spearman, p-values, regression,
 *              group t-test, correlation matrix
 * Graphs     : SVG scatter plot, SVG histogram, SVG correlation heatmap
 *
 * Compile : gcc attendance_analysis_v2.c -o analysis2 -lm
 * Run     : ./analysis2.exe students_v2.csv
 *
 * CSV example (first line is a header, all values are percentages 0-100):
 *   id,attendance,midsem,quiz
 *   101,92,85,78
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define MAX_STUDENTS   1000
#define LINE_LEN       256
#define THRESHOLD      75.0     /* attendance cut-off for group comparison */
#define ALPHA          0.05     /* significance level                      */
#define MAX_VARS       3        /* attendance + at most 2 marks columns    */

/* which marks the user wants to analyse */
enum { SEL_MID = 1, SEL_QUIZ = 2, SEL_BOTH = 3 };

/* ---------- data structures ---------- */

typedef struct {
    int    n;
    int    id[MAX_STUDENTS];
    double att[MAX_STUDENTS];     /* attendance %  */
    double mid[MAX_STUDENTS];     /* mid-sem marks % */
    double quiz[MAX_STUDENTS];    /* quiz marks %    */
} Dataset;

/* one column of data together with its name and graph colours */
typedef struct {
    const char   *name;
    const double *v;
    const char   *color;     /* point / bar colour      */
    const char   *line;      /* regression line colour  */
} Var;

typedef struct {
    double a;    /* intercept */
    double b;    /* slope     */
    double r2;   /* R squared */
} Regression;

/* ===================== 1. FILE HANDLING ===================== */

/* Splits a line in place at commas, semicolons or tabs. Returns field count. */
static int splitFields(char *line, char *fields[], int max)
{
    int k = 0;
    char *p = line;
    while (k < max) {
        fields[k++] = p;
        while (*p && *p != ',' && *p != ';' && *p != '\t') p++;
        if (*p == '\0') break;
        *p++ = '\0';
    }
    return k;
}

/* Converts text like " 85.5 " or "\"85\"" to a number. Returns 1 on success. */
static int parseNum(const char *s, double *out)
{
    while (*s == ' ' || *s == '"' || *s == '\'') s++;
    char *end;
    double v = strtod(s, &end);
    if (end == s) return 0;
    while (*end == ' ' || *end == '"' || *end == '\'') end++;
    if (*end != '\0') return 0;
    *out = v;
    return 1;
}

/* Reads the CSV into d. Returns number of valid records, or -1 on error.
   Accepts commas / semicolons / tabs, any line ending, a UTF-8 BOM,
   a header line, and ids that are text (e.g. CS101). */
int readData(const char *filename, Dataset *d)
{
    FILE *fp = fopen(filename, "rb");
    if (fp == NULL) {
        printf("Error: cannot open file '%s'\n", filename);
        return -1;
    }

    /* read the whole file into memory */
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    rewind(fp);
    char *buf = malloc(size + 1);
    if (buf == NULL) { fclose(fp); return -1; }
    size_t got 1= fread(buf, 1, size, fp);
    buf[got] = '\0';
    fclose(fp);

    char *start = buf;
    if ((unsigned char)start[0] == 0xEF && (unsigned char)start[1] == 0xBB &&
        (unsigned char)start[2] == 0xBF)
        start += 3;                                   /* skip UTF-8 BOM */

    int n = 0, //////////rowNo = 0, skipped = 0, first = 1;
    char firstLine[LINE_LEN] = "";

    for (char *line = strtok(start, "\r\n"); line != NULL; line = strtok(NULL, "\r\n")) {
        rowNo++;
        if (first) {                                  /* remember it for diagnostics */
            strncpy(firstLine, line, sizeof(firstLine) - 1);
            firstLine[sizeof(firstLine) - 1] = '\0';
        }

        char copy[LINE_LEN];
        strncpy(copy, line, sizeof(copy) - 1);
        copy[sizeof(copy) - 1] = '\0';

        char *f[8];
        int k = splitFields(copy, f, 8);
        double idv, att, mid, quiz;
        int ok = (k >= 4) && parseNum(f[1], &att) && parseNum(f[2], &mid) &&
                 parseNum(f[3], &quiz);

        if (!ok) {
            if (!first) {                             /* first bad line = header: ignore */
                printf("Warning: skipped row %d: %.60s\n", rowNo, line);
                skipped++;
            }
            first = 0;
            continue;
        }
        first = 0;

        if (att < 0 || att > 100 || mid < 0 || mid > 100 || quiz < 0 || quiz > 100) {
            printf("Warning: skipped row %d (value outside 0-100): %.60s\n", rowNo, line);
            skipped++;
            continue;
        }
        if (n >= MAX_STUDENTS) {
            printf("Warning: maximum of %d records reached\n", MAX_STUDENTS);
            break;
        }
        d->id[n]   = parseNum(f[0], &idv) ? (int)idv : n + 1;   /* text id -> row number */
        d->att[n]  = att;
        d->mid[n]  = mid;
        d->quiz[n] = quiz;
        n++;
    }
    free(buf);

    d->n = n;
    printf("Loaded %d records from '%s' (%d skipped).\n", n, filename, skipped);
    if (n == 0) {
        printf("\nNo usable data found. The first line of your file is:\n  %s\n", firstLine);
        printf("The program expects 4 columns:  id,attendance,midsem,quiz\n"
               "with attendance, midsem and quiz as numbers from 0 to 100.\n");
    }
    return n;
}

/* ===================== 2. SELECTION HELPERS ===================== */

const char *selName(int sel)
{
    if (sel == SEL_MID)  return "Mid-sem";
    if (sel == SEL_QUIZ) return "Quiz";
    return "Mid-sem + Quiz (both)";
}

const char *selTag(int sel)          /* used in output file names */
{
    if (sel == SEL_MID)  return "midsem";
    if (sel == SEL_QUIZ) return "quiz";
    return "both";
}

/* Fills out[] with the marks columns chosen by sel. Returns how many (1 or 2). */
int marksVars(const Dataset *d, int sel, Var out[])
{
    int k = 0;
    if (sel == SEL_MID || sel == SEL_BOTH) {
        out[k].name = "Mid-sem"; out[k].v = d->mid;
        out[k].color = "#2a7de1"; out[k].line = "#0b3d91";
        k++;
    }
    if (sel == SEL_QUIZ || sel == SEL_BOTH) {
        out[k].name = "Quiz"; out[k].v = d->quiz;
        out[k].color = "#f28e2b"; out[k].line = "#a14a00";
        k++;
    }
    return k;
}

/* Fills v[] with Attendance + the chosen marks columns. Returns the count. */
int analysisVars(const Dataset *d, int sel, Var v[])
{
    v[0].name = "Attendance"; v[0].v = d->att;
    v[0].color = "#4c9f70";   v[0].line = "#2d6a47";
    return 1 + marksVars(d, sel, v + 1);
}

/* ===================== 3. BASIC STATISTICS ===================== */

double mean(const double a[], int n)
{
    double sum = 0;
    for (int i = 0; i < n; i++) sum += a[i];
    return sum / n;
}

/* sample variance (n-1) */
double variance(const double a[], int n)
{
    double m = mean(a, n), sum = 0;
    for (int i = 0; i < n; i++) sum += (a[i] - m) * (a[i] - m);
    return sum / (n - 1);
}

double stdDev(const double a[], int n) { return sqrt(variance(a, n)); }

int cmpDouble(const void *p, const void *q)
{
    double d = *(const double *)p - *(const double *)q;
    return (d > 0) - (d < 0);
}

double median(const double a[], int n)
{
    double *t = malloc(n * sizeof(double));
    memcpy(t, a, n * sizeof(double));
    qsort(t, n, sizeof(double), cmpDouble);
    double m = (n % 2) ? t[n / 2] : (t[n / 2 - 1] + t[n / 2]) / 2.0;
    free(t);
    return m;
}

double minimum(const double a[], int n)
{
    double m = a[0];
    for (int i = 1; i < n; i++) if (a[i] < m) m = a[i];
    return m;
}

double maximum(const double a[], int n)
{
    double m = a[0];
    for (int i = 1; i < n; i++) if (a[i] > m) m = a[i];
    return m;
}

/* ===================== 4. CORRELATION & REGRESSION ===================== */

double pearson(const double x[], const double y[], int n)
{
    double mx = mean(x, n), my = mean(y, n);
    double sxy = 0, sxx = 0, syy = 0;
    for (int i = 0; i < n; i++) {
        sxy += (x[i] - mx) * (y[i] - my);
        sxx += (x[i] - mx) * (x[i] - mx);
        syy += (y[i] - my) * (y[i] - my);
    }
    if (sxx == 0 || syy == 0) return 0;      /* no variation */
    return sxy / sqrt(sxx * syy);
}

/* ---- ranks (ties get the average rank), needed for Spearman ---- */
typedef struct { double v; int idx; } Pair;

int cmpPair(const void *p, const void *q)
{
    double d = ((const Pair *)p)->v - ((const Pair *)q)->v;
    return (d > 0) - (d < 0);
}

void computeRanks(const double a[], int n, double rank[])
{
    Pair *p = malloc(n * sizeof(Pair));
    for (int i = 0; i < n; i++) { p[i].v = a[i]; p[i].idx = i; }
    qsort(p, n, sizeof(Pair), cmpPair);

    int i = 0;
    while (i < n) {
        int j = i;
        while (j + 1 < n && p[j + 1].v == p[i].v) j++;
        double avg = (i + j) / 2.0 + 1;          /* ranks start at 1 */
        for (int k = i; k <= j; k++) rank[p[k].idx] = avg;
        i = j + 1;
    }
    free(p);
}

double spearman(const double x[], const double y[], int n)
{
    double *rx = malloc(n * sizeof(double));
    double *ry = malloc(n * sizeof(double));
    computeRanks(x, n, rx);
    computeRanks(y, n, ry);
    double r = pearson(rx, ry, n);
    free(rx); free(ry);
    return r;
}

/* least squares: y = a + b x */
Regression linearRegression(const double x[], const double y[], int n)
{
    Regression reg;
    double mx = mean(x, n), my = mean(y, n);
    double sxy = 0, sxx = 0;
    for (int i = 0; i < n; i++) {
        sxy += (x[i] - mx) * (y[i] - my);
        sxx += (x[i] - mx) * (x[i] - mx);
    }
    reg.b  = (sxx == 0) ? 0 : sxy / sxx;
    reg.a  = my - reg.b * mx;
    double r = pearson(x, y, n);
    reg.r2 = r * r;
    return reg;
}

/* Correlation matrix: M[i][j] = Pearson r between variable i and variable j */
void buildMatrix(const Var v[], int k, int n, double M[MAX_VARS][MAX_VARS])
{
    for (int i = 0; i < k; i++)
        for (int j = 0; j < k; j++)
            M[i][j] = (i == j) ? 1.0 : pearson(v[i].v, v[j].v, n);
}

/* ===================== 5. HYPOTHESIS TESTING ===================== */
/* p-value of Student's t distribution using the regularised incomplete
   beta function (continued-fraction method). */

static double betacf(double a, double b, double x)
{
    const int    MAXIT = 300;
    const double EPS = 3e-12, FPMIN = 1e-300;
    double qab = a + b, qap = a + 1, qam = a - 1;
    double c = 1, d = 1 - qab * x / qap;
    if (fabs(d) < FPMIN) d = FPMIN;
    d = 1 / d;
    double h = d;
    for (int m = 1; m <= MAXIT; m++) {
        int m2 = 2 * m;
        double aa = m * (b - m) * x / ((qam + m2) * (a + m2));
        d = 1 + aa * d; if (fabs(d) < FPMIN) d = FPMIN;
        c = 1 + aa / c; if (fabs(c) < FPMIN) c = FPMIN;
        d = 1 / d;
        h *= d * c;
        aa = -(a + m) * (qab + m) * x / ((a + m2) * (qap + m2));
        d = 1 + aa * d; if (fabs(d) < FPMIN) d = FPMIN;
        c = 1 + aa / c; if (fabs(c) < FPMIN) c = FPMIN;
        d = 1 / d;
        double del = d * c;
        h *= del;
        if (fabs(del - 1) < EPS) break;
    }
    return h;
}

static double betai(double a, double b, double x)
{
    if (x <= 0) return 0;
    if (x >= 1) return 1;
    double bt = exp(lgamma(a + b) - lgamma(a) - lgamma(b)
                    + a * log(x) + b * log(1 - x));
    if (x < (a + 1) / (a + b + 2))
        return bt * betacf(a, b, x) / a;
    return 1 - bt * betacf(b, a, 1 - x) / b;
}

/* two-tailed p-value for a t statistic with df degrees of freedom */
double pValueT(double t, double df)
{
    return betai(df / 2.0, 0.5, df / (df + t * t));
}

/* Test H0: true correlation = 0.  Returns p-value, fills t statistic. */
double correlationTest(double r, int n, double *tStat)
{
    double df = n - 2;
    double denom = 1 - r * r;
    if (denom < 1e-15) { *tStat = INFINITY; return 0; }
    *tStat = r * sqrt(df / denom);
    return pValueT(*tStat, df);
}

/* Welch two-sample t-test. Returns p-value; fills t and df. */
double welchTTest(const double g1[], int n1, const double g2[], int n2,
                  double *t, double *df)
{
    double m1 = mean(g1, n1), m2 = mean(g2, n2);
    double v1 = variance(g1, n1) / n1;
    double v2 = variance(g2, n2) / n2;
    *t  = (m1 - m2) / sqrt(v1 + v2);
    *df = (v1 + v2) * (v1 + v2) /
          (v1 * v1 / (n1 - 1) + v2 * v2 / (n2 - 1));
    return pValueT(*t, *df);
}

/* ===================== 6. INTERPRETATION HELPERS ===================== */

/* prints tiny p-values as '< 0.0001' instead of 0.000000 */
void formatP(double p, char *buf)
{
    if (p < 0.0001) strcpy(buf, "< 0.0001");
    else            sprintf(buf, "%.4f", p);
}

const char *strengthWord(double r)
{
    double a = fabs(r);
    if (a < 0.2) return "very weak";
    if (a < 0.4) return "weak";
    if (a < 0.6) return "moderate";
    if (a < 0.8) return "strong";
    return "very strong";
}

const char *directionWord(double r)
{
    return (r >= 0) ? "positive" : "negative";
}

/* ===================== 7. TEXT REPORT FUNCTIONS ===================== */
/* All take a FILE* so the same code prints to the screen or to a report file */

/* prints one row of the descriptive table; f is the statistic function */
static void statRow(FILE *out, const char *label,
                    double (*f)(const double[], int),
                    const Var v[], int k, int n)
{
    fprintf(out, "%-10s", label);
    for (int i = 0; i < k; i++) fprintf(out, " %12.2f", f(v[i].v, n));
    fprintf(out, "\n");
}

void showDescriptive(FILE *out, const Dataset *d, int sel)
{
    Var v[MAX_VARS];
    int k = analysisVars(d, sel, v);

    fprintf(out, "\n=== DESCRIPTIVE STATISTICS (n = %d, marks: %s) ===\n",
            d->n, selName(sel));
    fprintf(out, "%-10s", "");
    for (int i = 0; i < k; i++) fprintf(out, " %12s", v[i].name);
    fprintf(out, "\n");
    statRow(out, "Mean",    mean,    v, k, d->n);
    statRow(out, "Median",  median,  v, k, d->n);
    statRow(out, "Std Dev", stdDev,  v, k, d->n);
    statRow(out, "Minimum", minimum, v, k, d->n);
    statRow(out, "Maximum", maximum, v, k, d->n);
}

void showCorrelation(FILE *out, const Dataset *d, int sel)
{
    Var m[2];
    int nm = marksVars(d, sel, m);

    fprintf(out, "\n=== CORRELATION ANALYSIS (marks: %s) ===\n", selName(sel));
    for (int i = 0; i < nm; i++) {
        double r  = pearson(d->att, m[i].v, d->n);
        double rs = spearman(d->att, m[i].v, d->n);
        double t, p = correlationTest(r, d->n, &t);
        char ps[32];
        formatP(p, ps);

        fprintf(out, "\n--- Attendance vs %s ---\n", m[i].name);
        fprintf(out, "Pearson  r   = %.4f\n", r);
        fprintf(out, "Spearman rho = %.4f  (rank based, less affected by outliers)\n", rs);
        fprintf(out, "t statistic = %.4f, df = %d, p-value = %s\n", t, d->n - 2, ps);
        fprintf(out, "Interpretation: a %s %s correlation. ",
                strengthWord(r), directionWord(r));
        if (p < ALPHA)
            fprintf(out, "Statistically significant at the %.0f%% level.\n", ALPHA * 100);
        else
            fprintf(out, "NOT statistically significant at the %.0f%% level.\n", ALPHA * 100);
    }
    fprintf(out, "\nNote: correlation does not prove that attendance CAUSES better marks;\n"
                 "other factors (study habits, ability, motivation) may be involved.\n");
}

void showRegression(FILE *out, const Dataset *d, int sel)
{
    Var m[2];
    int nm = marksVars(d, sel, m);
    double ex[] = {50, 60, 75, 90, 100};

    fprintf(out, "\n=== LINEAR REGRESSION (marks: %s) ===\n", selName(sel));
    for (int i = 0; i < nm; i++) {
        Regression g = linearRegression(d->att, m[i].v, d->n);
        fprintf(out, "\n--- %s = a + b * Attendance ---\n", m[i].name);
        fprintf(out, "%s = %.4f + %.4f * Attendance\n", m[i].name, g.a, g.b);
        fprintf(out, "R-squared = %.4f\n", g.r2);
        fprintf(out, "Each extra 1%% of attendance is associated with %+.3f %s marks; "
                     "attendance explains %.1f%% of the variation.\n",
                g.b, m[i].name, g.r2 * 100);
        fprintf(out, "Predictions:\n");
        for (int j = 0; j < 5; j++) {
            double pred = g.a + g.b * ex[j];
            if (pred > 100) pred = 100;
            if (pred < 0)   pred = 0;
            fprintf(out, "  Attendance %5.1f%%  ->  predicted %s %.1f\n",
                    ex[j], m[i].name, pred);
        }
    }
}

/* t-test: marks of students with attendance >= THRESHOLD vs below it */
static void groupComparison(FILE *out, const double att[], const Var *mk, int n)
{
    double *hi = malloc(n * sizeof(double));
    double *lo = malloc(n * sizeof(double));
    int nh = 0, nl = 0;

    for (int i = 0; i < n; i++) {
        if (att[i] >= THRESHOLD) hi[nh++] = mk->v[i];
        else                     lo[nl++] = mk->v[i];
    }

    fprintf(out, "\n--- %s marks ---\n", mk->name);
    if (nh < 2 || nl < 2) {
        fprintf(out, "Each group needs at least 2 students for a t-test "
                     "(high: %d, low: %d).\n", nh, nl);
        free(hi); free(lo);
        return;
    }

    double mh = mean(hi, nh), ml = mean(lo, nl);
    double t, df, p = welchTTest(hi, nh, lo, nl, &t, &df);
    char ps[32];
    formatP(p, ps);

    fprintf(out, "%-26s %8s %10s %8s\n", "Group", "Count", "Avg marks", "Std dev");
    fprintf(out, "%-26s %8d %10.2f %8.2f\n", "Attendance >= cut-off", nh, mh, stdDev(hi, nh));
    fprintf(out, "%-26s %8d %10.2f %8.2f\n", "Attendance <  cut-off", nl, ml, stdDev(lo, nl));
    fprintf(out, "Difference in means = %.2f marks\n", mh - ml);
    fprintf(out, "Welch t = %.4f, df = %.1f, p-value = %s\n", t, df, ps);
    fprintf(out, "Interpretation: students with attendance >= %.0f%% scored %.2f marks %s "
                 "on average; this difference %s statistically significant.\n",
            THRESHOLD, fabs(mh - ml), (mh >= ml) ? "higher" : "lower",
            (p < ALPHA) ? "IS" : "is NOT");

    free(hi); free(lo);
}

void showGroupComparison(FILE *out, const Dataset *d, int sel)
{
    Var m[2];
    int nm = marksVars(d, sel, m);
    fprintf(out, "\n=== GROUP COMPARISON (cut-off %.0f%% attendance, marks: %s) ===",
            THRESHOLD, selName(sel));
    for (int i = 0; i < nm; i++)
        groupComparison(out, d->att, &m[i], d->n);
}

void showMatrix(FILE *out, const Dataset *d, int sel)
{
    Var v[MAX_VARS];
    double M[MAX_VARS][MAX_VARS];
    int k = analysisVars(d, sel, v);
    buildMatrix(v, k, d->n, M);

    fprintf(out, "\n=== CORRELATION MATRIX (Pearson r, marks: %s) ===\n", selName(sel));
    fprintf(out, "%-12s", "");
    for (int j = 0; j < k; j++) fprintf(out, " %12s", v[j].name);
    fprintf(out, "\n");
    for (int i = 0; i < k; i++) {
        fprintf(out, "%-12s", v[i].name);
        for (int j = 0; j < k; j++) fprintf(out, " %12.4f", M[i][j]);
        fprintf(out, "\n");
    }
    fprintf(out, "(+1 = perfect positive, 0 = no linear relation, -1 = perfect negative)\n");
}

/* ===================== 8. SVG GRAPHS ===================== */

/* ---------- 8a. SCATTER PLOT (attendance on x, marks on y) ---------- */

#define SW 760
#define SH 560
#define SL 80
#define SR 40
#define ST 70
#define SB 80
#define SPW (SW - SL - SR)       /* plot area width  */
#define SPH (SH - ST - SB)       /* plot area height */
#define SX(v) (SL + (v) / 100.0 * SPW)
#define SY(v) (ST + SPH - (v) / 100.0 * SPH)

/* draws the regression line, clipped so it stays inside 0-100 marks */
static void drawRegLine(FILE *f, Regression g, const char *color)
{
    double x1 = 0, x2 = 100;
    double y1 = g.a + g.b * x1, y2 = g.a + g.b * x2;
    if (g.b != 0) {
        if (y1 < 0)   { y1 = 0;   x1 = (0 - g.a) / g.b; }
        if (y1 > 100) { y1 = 100; x1 = (100 - g.a) / g.b; }
        if (y2 < 0)   { y2 = 0;   x2 = (0 - g.a) / g.b; }
        if (y2 > 100) { y2 = 100; x2 = (100 - g.a) / g.b; }
    }
    fprintf(f, "<line x1='%.1f' y1='%.1f' x2='%.1f' y2='%.1f' stroke='%s' "
               "stroke-width='2.5'/>\n", SX(x1), SY(y1), SX(x2), SY(y2), color);
}

int svgScatter(const char *filename, const double att[], const Var v[], int nv, int n)
{
    FILE *f = fopen(filename, "w");
    if (f == NULL) {
        printf("Error: cannot create '%s'\n", filename);
        return 0;
    }

    fprintf(f, "<svg xmlns='http://www.w3.org/2000/svg' width='%d' height='%d' "
               "viewBox='0 0 %d %d' font-family='Arial, sans-serif'>\n", SW, SH, SW, SH);
    fprintf(f, "<rect width='%d' height='%d' fill='white'/>\n", SW, SH);

    /* title */
    char title[128] = "Attendance vs ";
    for (int j = 0; j < nv; j++) {
        if (j > 0) strcat(title, " and ");
        strcat(title, v[j].name);
    }
    strcat(title, " Marks");
    fprintf(f, "<text x='%d' y='32' font-size='20' font-weight='bold' text-anchor='middle'>"
               "%s</text>\n", SW / 2, title);
    fprintf(f, "<text x='%d' y='54' font-size='14' fill='#444' text-anchor='middle'>"
               "Scatter plot with regression line(s), n = %d</text>\n", SW / 2, n);

    /* grid lines + tick labels */
    for (int t = 0; t <= 100; t += 10) {
        fprintf(f, "<line x1='%.1f' y1='%d' x2='%.1f' y2='%d' stroke='#e5e5e5'/>\n",
                SX(t), ST, SX(t), ST + SPH);
        fprintf(f, "<line x1='%d' y1='%.1f' x2='%d' y2='%.1f' stroke='#e5e5e5'/>\n",
                SL, SY(t), SL + SPW, SY(t));
        fprintf(f, "<text x='%.1f' y='%d' font-size='12' text-anchor='middle'>%d</text>\n",
                SX(t), ST + SPH + 20, t);
        fprintf(f, "<text x='%d' y='%.1f' font-size='12' text-anchor='end'>%d</text>\n",
                SL - 8, SY(t) + 4, t);
    }

    /* attendance cut-off line */
    fprintf(f, "<line x1='%.1f' y1='%d' x2='%.1f' y2='%d' stroke='#999' "
               "stroke-dasharray='5,4'/>\n", SX(THRESHOLD), ST, SX(THRESHOLD), ST + SPH);
    fprintf(f, "<text x='%.1f' y='%d' font-size='11' fill='#777' text-anchor='middle'>"
               "%.0f%% cut-off</text>\n", SX(THRESHOLD), ST - 6, THRESHOLD);

    /* frame and axis titles */
    fprintf(f, "<rect x='%d' y='%d' width='%d' height='%d' fill='none' stroke='black'/>\n",
            SL, ST, SPW, SPH);
    fprintf(f, "<text x='%d' y='%d' font-size='15' text-anchor='middle'>Attendance (%%)</text>\n",
            SL + SPW / 2, SH - 25);
    fprintf(f, "<text x='22' y='%d' font-size='15' text-anchor='middle' "
               "transform='rotate(-90 22 %d)'>Marks (%%)</text>\n",
            ST + SPH / 2, ST + SPH / 2);

    /* points + regression line for each marks column */
    for (int j = 0; j < nv; j++) {
        for (int i = 0; i < n; i++)
            fprintf(f, "<circle cx='%.1f' cy='%.1f' r='4.5' fill='%s' "
                       "fill-opacity='0.6' stroke='%s'/>\n",
                    SX(att[i]), SY(v[j].v[i]), v[j].color, v[j].line);
        drawRegLine(f, linearRegression(att, v[j].v, n), v[j].line);
    }

    /* legend (top-left corner, usually the emptiest part of the plot) */
    fprintf(f, "<rect x='%d' y='%d' width='255' height='%d' fill='white' "
               "fill-opacity='0.92' stroke='#bbb'/>\n", SL + 10, ST + 10, 14 + 22 * nv);
    for (int j = 0; j < nv; j++) {
        double r = pearson(att, v[j].v, n);
        int cy = ST + 28 + 22 * j;
        fprintf(f, "<circle cx='%d' cy='%d' r='4.5' fill='%s' stroke='%s'/>"
                   "<line x1='%d' y1='%d' x2='%d' y2='%d' stroke='%s' stroke-width='2.5'/>"
                   "<text x='%d' y='%d' font-size='12'>%s: r = %.2f, R&#178; = %.2f</text>\n",
                SL + 24, cy, v[j].color, v[j].line,
                SL + 34, cy, SL + 52, cy, v[j].line,
                SL + 60, cy + 4, v[j].name, r, r * r);
    }

    fprintf(f, "</svg>\n");
    fclose(f);
    printf("Scatter plot saved to '%s' (open it in a web browser).\n", filename);
    return 1;
}

/* ---------- 8b. HISTOGRAM (one panel per variable) ---------- */

#define HP_W   400            /* width of one panel  */
#define HP_H   390            /* height of one panel */
#define HL     55
#define HT     60
#define HPW    325            /* plot width  */
#define HPH    255            /* plot height */
#define NBINS  10             /* bins of width 10: 0-10, 10-20, ... 90-100 */

static void histPanel(FILE *f, int ox, const Var *v, int n)
{
    int bins[NBINS] = {0};
    for (int i = 0; i < n; i++) {
        int b = (int)(v->v[i] / 10.0);
        if (b > NBINS - 1) b = NBINS - 1;      /* 100 goes into the last bin */
        if (b < 0) b = 0;
        bins[b]++;
    }

    int maxc = 0;
    for (int b = 0; b < NBINS; b++) if (bins[b] > maxc) maxc = bins[b];
    int step = (maxc <= 10) ? 1 : (maxc <= 20 ? 2 : 5);
    int ymax = ((maxc + step) / step) * step;   /* always a little head-room above the tallest bar */

    double bw = HPW / (double)NBINS;
    double m = mean(v->v, n), sd = stdDev(v->v, n);

    fprintf(f, "<g transform='translate(%d,0)'>\n", ox);
    fprintf(f, "<text x='%d' y='26' font-size='17' font-weight='bold' "
               "text-anchor='middle'>%s</text>\n", HP_W / 2, v->name);
    fprintf(f, "<text x='%d' y='45' font-size='12' fill='#444' text-anchor='middle'>"
               "n = %d, mean = %.1f, sd = %.1f</text>\n", HP_W / 2, n, m, sd);

    /* horizontal grid lines + y labels */
    for (int t = 0; t <= ymax; t += step) {
        double y = HT + HPH - (double)t * HPH / ymax;
        fprintf(f, "<line x1='%d' y1='%.1f' x2='%d' y2='%.1f' stroke='#e5e5e5'/>\n",
                HL, y, HL + HPW, y);
        fprintf(f, "<text x='%d' y='%.1f' font-size='11' text-anchor='end'>%d</text>\n",
                HL - 6, y + 4, t);
    }

    /* bars with count labels */
    for (int b = 0; b < NBINS; b++) {
        double h = (double)bins[b] * HPH / ymax;
        double x = HL + b * bw + 1;
        double y = HT + HPH - h;
        if (bins[b] > 0) {
            fprintf(f, "<rect x='%.1f' y='%.1f' width='%.1f' height='%.1f' "
                       "fill='%s' fill-opacity='0.8' stroke='%s'/>\n",
                    x, y, bw - 2, h, v->color, v->line);
            fprintf(f, "<text x='%.1f' y='%.1f' font-size='11' text-anchor='middle'>%d</text>\n",
                    x + (bw - 2) / 2, y - 4, bins[b]);
        }
    }

    /* x tick labels */
    for (int t = 0; t <= 100; t += 10)
        fprintf(f, "<text x='%.1f' y='%d' font-size='11' text-anchor='middle'>%d</text>\n",
                HL + t / 100.0 * HPW, HT + HPH + 17, t);

    /* mean line */
    double mx = HL + m / 100.0 * HPW;
    fprintf(f, "<line x1='%.1f' y1='%d' x2='%.1f' y2='%d' stroke='#d62728' "
               "stroke-width='2' stroke-dasharray='6,4'/>\n", mx, HT, mx, HT + HPH);
    fprintf(f, "<text x='%.1f' y='%d' font-size='11' fill='#d62728' text-anchor='%s'>"
               "mean</text>\n", mx + ((m > 80) ? -4 : 4), HT + 12, (m > 80) ? "end" : "start");

    /* frame + axis titles */
    fprintf(f, "<rect x='%d' y='%d' width='%d' height='%d' fill='none' stroke='black'/>\n",
            HL, HT, HPW, HPH);
    fprintf(f, "<text x='%d' y='%d' font-size='13' text-anchor='middle'>%s (%%)</text>\n",
            HL + HPW / 2, HT + HPH + 42, v->name);
    fprintf(f, "<text x='16' y='%d' font-size='13' text-anchor='middle' "
               "transform='rotate(-90 16 %d)'>Number of students</text>\n",
            HT + HPH / 2, HT + HPH / 2);
    fprintf(f, "</g>\n");
}

int svgHistogram(const char *filename, const Dataset *d, int sel)
{
    FILE *f = fopen(filename, "w");
    if (f == NULL) {
        printf("Error: cannot create '%s'\n", filename);
        return 0;
    }
    Var v[MAX_VARS];
    int k = analysisVars(d, sel, v);
    int W = HP_W * k;

    fprintf(f, "<svg xmlns='http://www.w3.org/2000/svg' width='%d' height='%d' "
               "viewBox='0 0 %d %d' font-family='Arial, sans-serif'>\n", W, HP_H, W, HP_H);
    fprintf(f, "<rect width='%d' height='%d' fill='white'/>\n", W, HP_H);
    for (int i = 0; i < k; i++)
        histPanel(f, i * HP_W, &v[i], d->n);
    fprintf(f, "</svg>\n");
    fclose(f);
    printf("Histogram saved to '%s' (open it in a web browser).\n", filename);
    return 1;
}

/* ---------- 8c. CORRELATION HEATMAP ---------- */

/* blue (-1) -> white (0) -> red (+1) */
static void heatColor(double r, char *buf)
{
    double a = fabs(r);
    if (a > 1) a = 1;
    int R, G, B;
    if (r >= 0) {                         /* white -> red   (214,39,40)  */
        R = (int)(255 + (214 - 255) * a);
        G = (int)(255 + (39  - 255) * a);
        B = (int)(255 + (40  - 255) * a);
    } else {                              /* white -> blue  (31,119,180) */
        R = (int)(255 + (31  - 255) * a);
        G = (int)(255 + (119 - 255) * a);
        B = (int)(255 + (180 - 255) * a);
    }
    sprintf(buf, "rgb(%d,%d,%d)", R, G, B);
}

int svgHeatmap(const char *filename, const Dataset *d, int sel)
{
    Var v[MAX_VARS];
    double M[MAX_VARS][MAX_VARS];
    int k = analysisVars(d, sel, v);
    buildMatrix(v, k, d->n, M);

    FILE *f = fopen(filename, "w");
    if (f == NULL) {
        printf("Error: cannot create '%s'\n", filename);
        return 0;
    }

    const int cell = 120, L = 140, T = 110, R = 130, B = 40;
    int W = L + k * cell + R;
    int H = T + k * cell + B;

    fprintf(f, "<svg xmlns='http://www.w3.org/2000/svg' width='%d' height='%d' "
               "viewBox='0 0 %d %d' font-family='Arial, sans-serif'>\n", W, H, W, H);
    fprintf(f, "<defs><linearGradient id='cb' x1='0' y1='0' x2='0' y2='1'>"
               "<stop offset='0' stop-color='rgb(214,39,40)'/>"
               "<stop offset='0.5' stop-color='white'/>"
               "<stop offset='1' stop-color='rgb(31,119,180)'/>"
               "</linearGradient></defs>\n");
    fprintf(f, "<rect width='%d' height='%d' fill='white'/>\n", W, H);
    fprintf(f, "<text x='%d' y='34' font-size='20' font-weight='bold' text-anchor='middle'>"
               "Correlation Heatmap (Pearson r)</text>\n", W / 2);
    fprintf(f, "<text x='%d' y='56' font-size='13' fill='#444' text-anchor='middle'>"
               "Marks: %s, n = %d students</text>\n", W / 2, selName(sel), d->n);

    /* column and row labels */
    for (int j = 0; j < k; j++) {
        fprintf(f, "<text x='%d' y='%d' font-size='14' font-weight='bold' "
                   "text-anchor='middle'>%s</text>\n",
                L + j * cell + cell / 2, T - 14, v[j].name);
        fprintf(f, "<text x='%d' y='%d' font-size='14' font-weight='bold' "
                   "text-anchor='end'>%s</text>\n",
                L - 12, T + j * cell + cell / 2 + 5, v[j].name);
    }

    /* coloured cells with the r values */
    for (int i = 0; i < k; i++) {
        for (int j = 0; j < k; j++) {
            char col[32];
            heatColor(M[i][j], col);
            fprintf(f, "<rect x='%d' y='%d' width='%d' height='%d' fill='%s' "
                       "stroke='white' stroke-width='3'/>\n",
                    L + j * cell, T + i * cell, cell, cell, col);
            fprintf(f, "<text x='%d' y='%d' font-size='22' font-weight='bold' "
                       "text-anchor='middle' fill='%s'>%.2f</text>\n",
                    L + j * cell + cell / 2, T + i * cell + cell / 2 + 8,
                    (fabs(M[i][j]) > 0.65) ? "white" : "#222", M[i][j]);
        }
    }

    /* colour bar */
    int bx = L + k * cell + 35;
    fprintf(f, "<rect x='%d' y='%d' width='22' height='%d' fill='url(#cb)' "
               "stroke='#888'/>\n", bx, T, k * cell);
    fprintf(f, "<text x='%d' y='%d' font-size='12'>+1</text>\n", bx + 30, T + 10);
    fprintf(f, "<text x='%d' y='%d' font-size='12'>0</text>\n", bx + 30, T + k * cell / 2 + 4);
    fprintf(f, "<text x='%d' y='%d' font-size='12'>-1</text>\n", bx + 30, T + k * cell);

    fprintf(f, "</svg>\n");
    fclose(f);
    printf("Heatmap saved to '%s' (open it in a web browser).\n", filename);
    return 1;
}

/* ===================== 9. SAVE EVERYTHING ===================== */

/* Builds a file name such as "scatter_both.svg" from a prefix and selection */
static void makeName(char *buf, const char *prefix, int sel, const char *ext)
{
    sprintf(buf, "%s_%s.%s", prefix, selTag(sel), ext);
}

void saveScatter(const Dataset *d, int sel)
{
    Var m[2];
    char fn[64];
    int nm = marksVars(d, sel, m);
    makeName(fn, "scatter", sel, "svg");
    svgScatter(fn, d->att, m, nm, d->n);
}

void saveHistogram(const Dataset *d, int sel)
{
    char fn[64];
    makeName(fn, "histogram", sel, "svg");
    svgHistogram(fn, d, sel);
}

void saveHeatmap(const Dataset *d, int sel)
{
    char fn[64];
    makeName(fn, "heatmap", sel, "svg");
    svgHeatmap(fn, d, sel);
}

void saveReport(const Dataset *d, int sel)
{
    char fn[64];
    makeName(fn, "report", sel, "txt");
    FILE *f = fopen(fn, "w");
    if (f == NULL) {
        printf("Error: cannot create '%s'\n", fn);
        return;
    }
    fprintf(f, "ATTENDANCE vs ACADEMIC PERFORMANCE - ANALYSIS REPORT\n");
    fprintf(f, "Marks analysed: %s\n", selName(sel));
    fprintf(f, "====================================================\n");
    showDescriptive(f, d, sel);
    showCorrelation(f, d, sel);
    showMatrix(f, d, sel);
    showRegression(f, d, sel);
    showGroupComparison(f, d, sel);
    fclose(f);
    printf("Full report saved to '%s'.\n", fn);
}

/* ===================== 10. MENU ===================== */

/* Reads one integer from the keyboard. Returns -1 for bad input, 0 at end of input. */
static int readInt(void)
{
    char buf[64];
    int v;
    if (fgets(buf, sizeof(buf), stdin) == NULL) return 0;   /* end of input */
    if (sscanf(buf, "%d", &v) != 1) return -1;
    return v;
}

int chooseMarks(int current)
{
    printf("\nWhich marks do you want to analyse?\n");
    printf("  1. Mid-sem marks\n");
    printf("  2. Quiz marks\n");
    printf("  3. Both (statistics for each, graphs show both)\n");
    printf("Choice: ");
    int c = readInt();
    if (c >= 1 && c <= 3) {
        printf("Now analysing: %s\n", selName(c));
        return c;
    }
    printf("Invalid choice, keeping: %s\n", selName(current));
    return current;
}

void printMenu(int sel)
{
    printf("\n========== ATTENDANCE vs PERFORMANCE ==========\n");
    printf(" Currently analysing: %s\n", selName(sel));
    printf("-----------------------------------------------\n");
    printf("  1. Change marks (mid-sem / quiz / both)\n");
    printf("  2. Descriptive statistics\n");
    printf("  3. Correlation analysis (Pearson, Spearman, p-value)\n");
    printf("  4. Correlation matrix (numbers)\n");
    printf("  5. Linear regression and predictions\n");
    printf("  6. High vs low attendance group comparison (t-test)\n");
    printf("  7. SVG scatter plot\n");
    printf("  8. SVG histogram\n");
    printf("  9. SVG correlation heatmap\n");
    printf(" 10. Run everything (all results + all graphs + report)\n");
    printf("  0. Exit\n");
    printf("Choice: ");
}

int main(int argc, char *argv[])
{
    setvbuf(stdout, NULL, _IONBF, 0);      /* show output immediately (MSYS2/Windows) */

    static Dataset ds;
    char path[256];
    strncpy(path, (argc > 1) ? argv[1] : "students_v2.csv", sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';

    /* if the file is not found, let the user type another path */
    int n = readData(path, &ds);
    while (n < 0) {
        printf("Enter the full path of the CSV file (or q to quit): ");
        if (fgets(path, sizeof(path), stdin) == NULL) return 1;
        path[strcspn(path, "\r\n")] = '\0';
        if (strcmp(path, "q") == 0) return 1;
        n = readData(path, &ds);
    }
    if (n < 3) {
        printf("Need at least 3 valid records to analyse.\n");
        return 1;
    }

    int sel = chooseMarks(SEL_MID);
    int choice;
    do {
        printMenu(sel);
        choice = readInt();
        switch (choice) {
            case 1:  sel = chooseMarks(sel);                 break;
            case 2:  showDescriptive(stdout, &ds, sel);      break;
            case 3:  showCorrelation(stdout, &ds, sel);      break;
            case 4:  showMatrix(stdout, &ds, sel);           break;
            case 5:  showRegression(stdout, &ds, sel);       break;
            case 6:  showGroupComparison(stdout, &ds, sel);  break;
            case 7:  saveScatter(&ds, sel);                  break;
            case 8:  saveHistogram(&ds, sel);                break;
            case 9:  saveHeatmap(&ds, sel);                  break;
            case 10:
                showDescriptive(stdout, &ds, sel);
                showCorrelation(stdout, &ds, sel);
                showMatrix(stdout, &ds, sel);
                showRegression(stdout, &ds, sel);
                showGroupComparison(stdout, &ds, sel);
                saveScatter(&ds, sel);
                saveHistogram(&ds, sel);
                saveHeatmap(&ds, sel);
                saveReport(&ds, sel);
                break;
            case 0:  printf("Thank You. Attendace vs Marks correlelation by Aditya and Raj (B1)!\n");                   break;
            default: printf("Invalid choice, try again.\n");
        }
    } while (choice != 0);

    return 0;
}
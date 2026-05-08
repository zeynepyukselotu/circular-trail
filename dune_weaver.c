/*
 * Dune Weaver - Sand Table Control
 * Polar Coordinate System (Theta + Rho)
 * Translated to C from Python pattern_manager.py.
 *
 * Calibration:
 *   X50  = 1 full revolution (theta axis)
 *   Y    = rho axis (range -11 to +11, center Y0)
 *   $100 = 320 steps/mm (x_steps_per_mm)
 *   $101 = 287 steps/mm (y_steps_per_mm)
 *   gear_ratio = 10
 *
 * Build:
 *   gcc dune_weaver.c -o dune_weaver.exe -lm
 *
 * Usage:
 *   1. Manually move the stylus to center (Y0)
 *   2. Run the program and enter the COM port
 *   3. Select a pattern
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <windows.h>

/* ================================================================
 * CALIBRATION CONSTANTS
 * ================================================================ */
#define PI              3.14159265358979323846

/* X50 = 1 full revolution (theta axis) */
#define X_PER_REV       50.0

/* steps/mm values ($100 and $101) */
#define X_STEPS_PER_MM  320.0
#define Y_STEPS_PER_MM  287.0

/* Gear ratio (150/15 = 10) */
#define GEAR_RATIO      10.0

/* X and Y scaling factors (from Python pattern_manager.py) */
#define X_SCALING       2.0
#define Y_MAX_MM        36   /* radial distance from center to edge in mm */
#define KREMAYER_OFFSET 0.49  /* experimental, adjust as needed */

/* Movement speed (mm/min) */
#define FEED_RATE       200

/* Number of interpolation points per pattern */
#define STEPS           360

/* ================================================================
 * SERIAL PORT
 * ================================================================ */
HANDLE hSerial = INVALID_HANDLE_VALUE;

/* Current machine position (mirrors Python state.machine_x, state.machine_y) */
double machine_x = 0.0;
double machine_y = 0.0;

/* Current polar position (mirrors Python state.current_theta, state.current_rho) */
double current_theta = 0.0;
double current_rho   = 0.0;

int serial_open(const char *port) {
    char full_port[20];
    snprintf(full_port, sizeof(full_port), "\\\\.\\%s", port);
    hSerial = CreateFileA(full_port, GENERIC_READ | GENERIC_WRITE,
                          0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hSerial == INVALID_HANDLE_VALUE) {
        printf("ERROR: Could not open port %s!\n", port);
        return 0;
    }
    DCB dcb = {0};
    dcb.DCBlength = sizeof(dcb);
    GetCommState(hSerial, &dcb);
    dcb.BaudRate = CBR_115200;
    dcb.ByteSize = 8;
    dcb.StopBits = ONESTOPBIT;
    dcb.Parity   = NOPARITY;
    SetCommState(hSerial, &dcb);
    COMMTIMEOUTS t = {50, 10, 500, 10, 500};
    SetCommTimeouts(hSerial, &t);
    printf("Port %s opened.\n", port);
    Sleep(2000);
    return 1;
}

void serial_close(void) {
    if (hSerial != INVALID_HANDLE_VALUE) {
        CloseHandle(hSerial);
        hSerial = INVALID_HANDLE_VALUE;
    }
}

/* Send a command to GRBL and block until "ok" or "error" is received (30s timeout) */
void grbl_send(const char *cmd) {
    if (hSerial == INVALID_HANDLE_VALUE) return;
    char buf[128];
    snprintf(buf, sizeof(buf), "%s\n", cmd);
    DWORD written;
    WriteFile(hSerial, buf, (DWORD)strlen(buf), &written, NULL);
    char response[64] = {0};
    char c;
    DWORD rd;
    int idx = 0;
    DWORD start = GetTickCount();
    while (GetTickCount() - start < 30000) {
        ReadFile(hSerial, &c, 1, &rd, NULL);
        if (rd == 0) { Sleep(1); continue; }
        if (c == '\n') {
            response[idx] = '\0';
            if (strstr(response, "ok") || strstr(response, "error")) return;
            idx = 0;
        } else if (c != '\r' && idx < 63) {
            response[idx++] = c;
        }
    }
}

/* ================================================================
 * POLAR MOVEMENT FUNCTION
 * ================================================================
 *
 * Translated from Python _move_polar_sync().
 *
 * Kremayer (rack) compensation:
 *   When theta rotates, the rack mechanically shifts the rho position.
 *   To compensate, an offset proportional to the theta change is added
 *   to y_increment.
 *
 *   offset = x_increment * (x_total_steps * X_SCALING)
 *                        / (GEAR_RATIO * y_total_steps * Y_SCALING)
 *
 * Coordinates sent to GRBL:
 *   new_x = machine_x + x_increment  (absolute position)
 *   new_y = machine_y + y_increment  (absolute position, compensated)
 * ================================================================ */
void move_polar(double theta, double rho) {
    double delta_theta = theta - current_theta;
    double delta_rho   = rho   - current_rho;

    /* Theta movement in mm */
    double x_increment = delta_theta / (2.0 * PI) * X_PER_REV;

    /* Rho movement in mm */
    double y_increment = delta_rho * Y_MAX_MM;

    /* Kremayer compensation: rack shifts when theta rotates */
    double offset = x_increment * (X_STEPS_PER_MM * X_SCALING)
                    / (GEAR_RATIO * Y_STEPS_PER_MM * KREMAYER_OFFSET);
    y_increment += offset;

    double new_x = machine_x + x_increment;
    double new_y = machine_y + y_increment;

    char cmd[64];
    snprintf(cmd, sizeof(cmd), "G1 X%.3f Y%.3f F%d", new_x, new_y, FEED_RATE);
    grbl_send(cmd);

    current_theta = theta;
    current_rho   = rho;
    machine_x     = new_x;
    machine_y     = new_y;
}

/* ================================================================
 * PATTERNS
 * ================================================================ */

/*
 * 1. CIRCLE
 *    Rotates theta from 0 to 2*PI at a fixed rho.
 *    Kremayer compensation is applied automatically inside move_polar.
 */
void draw_circle(void) {
    printf("  Drawing circle...\n");
    int i;
    for (i = 0; i <= STEPS; i++) {
        double rho = 0.85;
        double theta = 2.0 * PI * i / STEPS;
        move_polar(theta, rho);
    }
    printf("  Circle complete.\n");
}

/*
 * 2. TRIANGLE
 *    3-sided polygon. Each side is a straight line in Cartesian space,
 *    then converted to polar coordinates.
 *    Theta continuity is preserved (prevents 2*PI wrap jumps).
 */
void draw_triangle(void) {
    printf("  Drawing triangle...\n");
    int sides    = 3;
    double rho   = 0.8;
    int seg      = STEPS / sides;
    int s, i;
    double theta_acc = PI / 2.0;  /* first corner at top */
    move_polar(theta_acc, 0.0);   /* rotate to start angle while at center */

    for (s = 0; s < sides; s++) {
        double a1 = 2.0 * PI * s       / sides + PI / 2.0;
        double a2 = 2.0 * PI * (s + 1) / sides + PI / 2.0;

        double x1 = rho * cos(a1);
        double y1 = rho * sin(a1);
        double x2 = rho * cos(a2);
        double y2 = rho * sin(a2);

        for (i = 0; i <= seg; i++) {
            double t  = (double)i / seg;
            double cx = x1 + (x2 - x1) * t;
            double cy = y1 + (y2 - y1) * t;

            /* Cartesian -> Polar */
            double r  = sqrt(cx*cx + cy*cy);
            double th = atan2(cy, cx);

            /* Theta continuity: prevent jumps larger than PI from previous value */
            while (th < theta_acc - PI) th += 2.0 * PI;
            while (th > theta_acc + PI) th -= 2.0 * PI;
            theta_acc = th;

            move_polar(theta_acc, r);
        }
    }
    printf("  Triangle complete.\n");
}

/*
 * 3. SQUARE
 *    4-sided polygon. Same logic as triangle with 4 sides.
 *    PI/4 starting angle added to rotate 45 degrees.
 */
void draw_square(void) {
    printf("  Drawing square...\n");
    int sides    = 4;
    double rho   = 0.75;
    int seg      = STEPS / sides;
    int s, i;
    double theta_acc = PI / 4.0;  /* first corner at 45 degrees */
    move_polar(theta_acc, 0.0);   /* rotate to start angle while at center */

    for (s = 0; s < sides; s++) {
        double a1 = 2.0 * PI * s       / sides + PI / 4.0;
        double a2 = 2.0 * PI * (s + 1) / sides + PI / 4.0;

        double x1 = rho * cos(a1);
        double y1 = rho * sin(a1);
        double x2 = rho * cos(a2);
        double y2 = rho * sin(a2);

        for (i = 0; i <= seg; i++) {
            double t  = (double)i / seg;
            double cx = x1 + (x2 - x1) * t;
            double cy = y1 + (y2 - y1) * t;

            double r  = sqrt(cx*cx + cy*cy);
            double th = atan2(cy, cx);

            while (th < theta_acc - PI) th += 2.0 * PI;
            while (th > theta_acc + PI) th -= 2.0 * PI;
            theta_acc = th;

            move_polar(theta_acc, r);
        }
    }
    printf("  Square complete.\n");
}

/*
 * 4. SPIRAL
 *    Archimedean spiral from center (rho=0) to edge (rho=0.85) over 3 full turns.
 *    Rho increases linearly with theta.
 */
void draw_spiral(void) {
    printf("  Drawing spiral...\n");
    int turns       = 3;
    int total_steps = STEPS * turns;
    int i;
    for (i = 0; i <= total_steps; i++) {
        double theta = 2.0 * PI * turns * i / total_steps;
        double rho   = 0.85 * i / total_steps;
        move_polar(theta, rho);
    }
    printf("  Spiral complete.\n");
}

/* Go home: move radially inward to center keeping the current angle */
void go_home(void) {
    printf("  Returning to center...\n");
    move_polar(current_theta, 0.0);
}

/* ================================================================
 * MENU
 * ================================================================ */
void print_menu(void) {
    printf("\n");
    printf("========================================\n");
    printf("   DUNE WEAVER - Sand Table Control\n");
    printf("========================================\n");
    printf("  1. Circle\n");
    printf("  2. Triangle\n");
    printf("  3. Square\n");
    printf("  4. Spiral\n");
    printf("  5. Go Home\n");
    printf("  0. Exit\n");
    printf("----------------------------------------\n");
    printf("Your choice: ");
}

/* ================================================================
 * MAIN
 * ================================================================ */
int main(void) {
    char port[10];
    int choice;

    printf("========================================\n");
    printf("   DUNE WEAVER - Starting\n");
    printf("========================================\n");
    printf("NOTE: Manually move the stylus to center (Y0) before starting!\n");
    printf("Enter Arduino COM port (e.g. COM8): ");
    scanf("%9s", port);

    if (!serial_open(port)) return 1;

    /* GRBL startup commands */
    grbl_send("$X");        /* clear alarm */
    grbl_send("G90");       /* absolute positioning mode */
    grbl_send("G21");       /* millimeter units */
    grbl_send("G92 X0 Y0"); /* set current position as origin */

    /* Reset state */
    machine_x     = 0.0;
    machine_y     = 0.0;
    current_theta = 0.0;
    current_rho   = 0.0;

    printf("GRBL ready! Select a pattern.\n");

    while (1) {
        print_menu();
        if (scanf("%d", &choice) != 1) { choice = -1; }
        switch (choice) {
            case 1: draw_circle();   break;
            case 2: draw_triangle(); break;
            case 3: draw_square();   break;
            case 4: draw_spiral();   break;
            case 5: go_home();       break;
            case 0:
                printf("Exiting...\n");
                go_home();
                serial_close();
                return 0;
            default:
                printf("Invalid selection.\n");
        }
    }

    serial_close();
    return 0;
}

// Latency tester v3 case - Waveshare RP2040-LCD-0.96 + ALS-PT19 + 6x6 button
// The screen faces you through the lid; the light sensor looks out of the flat
// back. Hold the back against the monitor. The button is soldered straight onto
// the board (legs in pins 21 GP16 and 23 GND, body towards the middle of the
// board) and pokes through a hole in the lid. The light sensor lies face-down in
// a pocket in the back, under the board.
//
// About 57 x 26 x 13 mm. Screwless: the lid's lip is a push fit (a drop of glue
// or a strip of tape if it's loose on your printer).
//
// Parts (set PART):
//   "base" - back: board ledge, USB-C opening, sensor pocket
//   "lid"  - front: screen window, button hole. Exported face-down, ready to print.
//   "all"  - assembly preview
//
// Wiring: no header pins on the board (solder wires into the holes from
// underneath). If your board has pins soldered on, set under = 9.5.
//
// CHECK BEFORE PRINTING - measure your board and set these if they differ:
//   scr_x    distance from the board's USB end to the centre of the screen
//   scr_y    offset of the screen centre across the board (0 = centred)
//   btn_x    distance from the board's USB end to the centre of the button stem
//   btn_y    stem centre from the board's centre line, towards the pin 21-40 edge
//   tact_h   button height, bottom of the body to the top of the stem
//   sens_x   where the sensor sits under the board, from the USB end - keep it
//   sens_y   clear of anything on the board's underside (battery socket)

PART = "all";
$fn = 40;

// ---------- board ----------
bd = [51, 21];          // board outline (x along the length, USB at x = 0)
bd_t = 1.6;             // PCB thickness
top_h = 3.6;            // tallest part on top (USB-C, screen, BOOT/RESET) + clearance
under = 4;              // space under the board for wire ends
fit = 0.3;              // board clearance
scr_x = 25.5;           // screen centre from the USB end
scr_y = 0;
scr_win = [24, 12.4];   // window at the screen (active area is 21.7 x 10.8)
usb_w = 12.4;           // USB-C plug overmould width
usb_h = 7.4;            // ... and height

// ---------- case ----------
wall = 2;
floor_t = 2;
lid_t = 1.8;
lip = 1.2;              // lid lip width
lip_d = 2.4;            // lid lip depth
lid_fit = 0.15;         // lip clearance (push fit)

// ---------- button (standard 6 x 6 mm tactile, 4 legs) on the board ----------
// Pins 21 and 23 are 5.08 mm apart; with those two legs in them the stem sits
// 3.25 mm in from the pin row.
btn_x = 25.5 + (24.13 + 19.05) / 2;     // = 47.1, midway between pins 21 and 23
btn_y = 8.89 - 3.25;                    // = 5.64
tact_h = 5;             // 5 mm button: stem top ends 0.4 mm under the lid face
btn_hole = 4.4;         // stem is 3.5 mm
btn_body = 6.6;         // clearance for the 6 x 6 body under the lid

// ---------- ALS-PT19 (Adafruit 2748) ----------
// Pocket: the breakout lies face-down on a ledge; a smaller recess under it
// clears the sensor chip; a 2 mm hole lets the light in; a peg goes through
// the breakout's mounting hole to line it up.
als = [8.0, 10.4];      // breakout size, long side across the case
sens_x = 21.9;          // pocket centre from the board's USB end
sens_y = 0;
als_fit = 0.3;
als_frame = 1.0;
als_skin = 1.0;         // back thickness under the chip
als_ledge = 1.8;        // height of the ledge the breakout rests on
als_recess = [6.6, 9.1];// chip recess
win_d = 2.0;            // light hole
win_off = [-1.4, 0];    // light hole from the pocket centre
peg_d = 1.5;            // locating peg
peg_off = [0.1, 3.3];
peg_h = 1.4;            // above the ledge (breakout PCB is 1.6)

// ---------- board ledge ----------
ledge_w = 2.0;          // ledge round the inside the board rests on
// gaps in the ledge for solder joints under the board (pins 31-36 sensor wires,
// pins 21-23 button legs), x from the board's USB end, on the pin 21-40 side
ledge_gaps = [[10, 26], [43, 51.5]];

// =====================================================================
bx0 = 1.0;                              // board starts 1 mm from the USB wall (connector overhang)
bx1 = bx0 + bd[0];
iy = bd[1]/2 + fit;                     // inner half width
Li = bx1 + fit;                         // inner length (end wall stops the board)
L = Li + 2*wall;
W = 2*iy + 2*wall;
bz0 = floor_t + under;                  // board bottom
bz1 = bz0 + bd_t;                       // board top
H = bz1 + top_h;                        // base height = lid underside
tact_c = [bx0 + btn_x, btn_y];
sens_c = [bx0 + sens_x, sens_y];
body_top = bz1 + 3.6;                   // top of the 6 x 6 body
usb_z = bz1 + 1.65;                     // USB-C centre (top-mounted receptacle)

module rbox(x0, y0, sx, sy, sz, r = 2) {
    hull() for (x = [x0 + r, x0 + sx - r], y = [y0 + r, y0 + sy - r])
        translate([x, y, 0]) cylinder(r = r, h = sz);
}

module base() {
    difference() {
        union() {
            difference() {
                rbox(-wall, -iy - wall, L, W, H, 2.5);
                translate([0, -iy, floor_t]) cube([Li, 2*iy, H]);
            }
            // ledge round the inside, the board rests on it
            difference() {
                translate([0, -iy, 0]) cube([Li, 2*iy, bz0]);
                translate([ledge_w, -iy + ledge_w, floor_t]) cube([Li - 2*ledge_w, 2*iy - 2*ledge_w, bz0]);
                for (g = ledge_gaps)
                    translate([bx0 + g[0], iy - ledge_w - 0.01, floor_t]) cube([g[1] - g[0], ledge_w + 0.02, bz0]);
            }
            // sensor frame (its top stays 1.6 mm under the board)
            fx = als[0] + 2*als_fit + 2*als_frame;
            fy = als[1] + 2*als_fit + 2*als_frame;
            translate([sens_c[0] - fx/2, sens_c[1] - fy/2, 0]) cube([fx, fy, floor_t + 2.4]);
        }
        // USB-C: open-topped slot in the end wall
        translate([-wall - 1, scr_y_usb() - usb_w/2, usb_z - usb_h/2]) cube([wall + 1.5, usb_w, H]);
        // sensor pocket: breakout on a ledge, chip recess under it, light hole
        px = als[0] + 2*als_fit;  py = als[1] + 2*als_fit;
        translate([sens_c[0] - px/2, sens_c[1] - py/2, als_ledge]) cube([px, py, 10]);
        translate([sens_c[0] - als_recess[0]/2, sens_c[1] - als_recess[1]/2, als_skin]) cube([als_recess[0], als_recess[1], 10]);
        translate([sens_c[0] + win_off[0], sens_c[1] + win_off[1], -1]) cylinder(d = win_d, h = als_skin + 1.01, $fn = 32);
        // ring on the back marking the sensor, for a foam/felt gasket
        translate([sens_c[0] + win_off[0], sens_c[1] + win_off[1], -0.01]) difference() { cylinder(d = 11, h = 0.4); cylinder(d = 10, h = 1); }
    }
    // locating peg for the breakout's mounting hole
    translate([sens_c[0] + peg_off[0], sens_c[1] + peg_off[1], als_skin])
        cylinder(d = peg_d, h = als_ledge - als_skin + peg_h, $fn = 24);
}

function scr_y_usb() = 0;   // USB-C is centred across the board

module lid() {
    difference() {
        union() {
            rbox(-wall, -iy - wall, L, W, lid_t, 2.5);
            // push-fit lip
            difference() {
                translate([lid_fit, -iy + lid_fit, -lip_d]) cube([Li - 2*lid_fit, 2*iy - 2*lid_fit, lip_d]);
                translate([lid_fit + lip, -iy + lid_fit + lip, -lip_d - 1]) cube([Li - 2*lid_fit - 2*lip, 2*iy - 2*lid_fit - 2*lip, lip_d + 1]);
            }
            // rails that hold the board down along its long edges
            for (s = [-1, 1])
                translate([bx0 + 2, s > 0 ? iy - lid_fit - 1.0 : -iy + lid_fit, -top_h]) cube([bd[0] - 4, 1.0, top_h]);
        }
        // screen window, chamfered towards the viewer
        translate([bx0 + scr_x, scr_y, -top_h - 1]) {
            translate([-scr_win[0]/2, -scr_win[1]/2, 0]) cube([scr_win[0], scr_win[1], top_h + 1 + lid_t + 1]);
            translate([0, 0, top_h + 1 + 0.4])
                hull() {
                    translate([-scr_win[0]/2, -scr_win[1]/2, 0]) cube([scr_win[0], scr_win[1], 0.01]);
                    translate([-scr_win[0]/2 - 1.6, -scr_win[1]/2 - 1.6, lid_t - 0.4 + 0.01]) cube([scr_win[0] + 3.2, scr_win[1] + 3.2, 0.5]);
                }
        }
        // USB-C notch in the lid edge (matches the slot in the base)
        translate([-wall - 1, -usb_w/2, -lip_d - 1]) cube([wall + 1 + lid_fit + lip + 0.1, usb_w, lid_t + lip_d + 2]);
        // button: hole for the stem, countersunk so a finger reaches it
        translate([tact_c[0], tact_c[1], -1]) cylinder(d = btn_hole, h = lid_t + 2);
        translate([tact_c[0], tact_c[1], lid_t - 1.0]) cylinder(d1 = btn_hole, d2 = btn_hole + 2.4, h = 1.01);
        // clear the button body (and the rail) around it
        translate([tact_c[0] - btn_body/2, tact_c[1] - btn_body/2, -top_h - 1]) cube([btn_body, btn_body, top_h + 1 + max(0, body_top - H) + 0.01]);
    }
}

// rough board + parts, for the preview only
module board_preview() {
    color("DarkGreen") translate([bx0, -bd[1]/2, bz0]) cube([bd[0], bd[1], bd_t]);
    color("Black") translate([bx0 + scr_x - 12.3, scr_y - 7, bz1]) cube([24.6, 14, 1.6]);
    color("Silver") translate([bx0 - 0.6, -4.5, bz1]) cube([7.5, 9, 3.2]);
    color("Black") translate([tact_c[0] - 3, tact_c[1] - 3, bz1]) cube([6, 6, 3.5]);
    color("DimGray") translate([tact_c[0], tact_c[1], bz1]) cylinder(d = 3.5, h = tact_h);
    color("Purple") translate([sens_c[0] - als[0]/2, sens_c[1] - als[1]/2, als_ledge]) cube([als[0], als[1], 1.6]);
}

if (PART == "base") base();
else if (PART == "lid") translate([0, 0, lid_t]) mirror([0, 0, 1]) lid();
else if (PART == "open") {
    color("SteelBlue") base();
    board_preview();
    color("LightGray", 0.9) translate([0, W + 6, lid_t]) mirror([0, 0, 1]) lid();
} else {
    color("SteelBlue") base();
    board_preview();
    color("LightGray", 0.85) translate([0, 0, H]) lid();
}

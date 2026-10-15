// Ultimatrix prototype. All dimensions in mm. LED face points towards z=0.
// Export with -D 'part="shell"' -D 'pitch=3' (or 2.5).
part = "assembly"; // shell, rear, grid, sample, assembly
pitch = 3;
pixels = 64;
panel = pixels*pitch;
panel_depth = 14; // verified for Cailiang P3 only
diffuser = 2.6;
gap = 3; // LED top to diffuser: optical trial required
wall = 2.4;
clearance = 0.4; // per side
grid_wall = 0.4; // use a 0.2 mm nozzle for the optical grid
outer = panel+12;
rear_clearance = 12; // provisional: measure controller and plugs before printing
depth = 1.2+diffuser+gap+panel_depth+rear_clearance;
rear_t = 2.4;
boss_xy = outer/2-4;
$fn = 32;

module rounded(w,h,r=3) {
    linear_extrude(h) offset(r=r) square([w-2*r,w-2*r],center=true);
}
module screw_positions() {
    for(x=[-1,1],y=[-1,1]) translate([x*boss_xy,y*boss_xy,0]) children();
}
module shell() {
    difference() {
        union() {
            difference() {
                // One millimetre front chamfer and soft outer corners.
                union() {
                    hull() {
                        rounded(outer-2,0.1,5);
                        translate([0,0,1]) rounded(outer,0.1,6);
                    }
                    translate([0,0,1]) rounded(outer,depth-1,6);
                }
                // Continuous opening avoids masking outer LED rows.
                translate([0,0,-0.1]) rounded(panel+0.2,depth+0.2,0.8);
                // Diffuser pocket. A gasket/spacer takes up thickness tolerance.
                translate([0,0,1.2]) rounded(panel+6.8,diffuser+0.3,1);
                // Main cavity; front pocket retains the diffuser.
                translate([0,0,1.2+diffuser+0.3])
                    rounded(outer-2*wall,depth,1.2);
            }
            screw_positions() translate([0,0,depth-9]) cylinder(d=6.4,h=9);
        }
        // Recessed lid: 0.3 mm perimeter shadow gap, flush with rear edge.
        translate([0,0,depth-rear_t]) rounded(outer-1.2,rear_t+0.1,5.4);
        // M3 plastic-thread pilot holes; tune to screws/material on a coupon.
        screw_positions() translate([0,0,depth-10]) cylinder(d=2.7,h=11);
        // Rear-facing cable exit, opening into the removable lid seam.
        translate([0,-outer/2,depth-3]) cube([14,10,6.2],center=true);
    }
}
module rear() {
    difference() {
        rounded(outer-1.8,rear_t,5.1);
        screw_positions() translate([0,0,-0.1]) cylinder(d=3.4,h=rear_t+0.2);
        // Ventilation slots; leave the centre available for adjustable fixtures.
        for(y=[-panel/2+15,panel/2-15],x=[-panel/3:8:panel/3])
            translate([x,y,rear_t/2]) cube([3,20,rear_t+1],center=true);
        // Universal mounting slots, not a claimed Matrix Portal hole pattern.
        for(x=[-35,35],y=[-25,0,25])
            translate([x,y,-0.1]) linear_extrude(rear_t+0.2)
                hull() for(dx=[-10,10]) translate([dx,0]) circle(d=3.4);
        // Two keyholes: screw head enters below, slides into upper neck.
        for(x=[-40,40]) translate([x,panel/2-38,-0.1])
            linear_extrude(rear_t+0.2) union() {
                circle(d=7); translate([-1.8,0]) square([3.6,9]);
            }
    }
}
module grid(n=64) {
    w=n*pitch;
    // Cells align to pixel centres at half-pitch from the panel edge.
    // Grid starts at LED-top height; an underside skirt needs measured LED gaps.
    union() {
        for(i=[0:n]) {
            translate([-w/2+i*pitch,0,gap/2]) cube([grid_wall,w+grid_wall,gap],center=true);
            translate([0,-w/2+i*pitch,gap/2]) cube([w+grid_wall,grid_wall,gap],center=true);
        }
    }
}
if(part=="shell") shell();
if(part=="rear") rear();
if(part=="grid") grid();
if(part=="sample") grid(8);
if(part=="assembly") {
    color("#30343b") shell();
    color([0.12,0.12,0.12,0.6]) translate([0,0,1.2]) rounded(panel+6.4,diffuser,1);
    color("#151515") translate([0,0,1.2+diffuser]) grid();
    color("#50555c") translate([0,0,depth+8]) rear();
}

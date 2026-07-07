/* D-OPT-SWITCH-JUMP-TABLE (c70) VDBE-dispatch shape: a ~350-case DENSE
   contiguous switch (values 0..349) evaluated inside a loop — the exact shape
   of sqlite's VDBE opcode dispatch (sqlite3VdbeExec), which is the sqlite-green
   blocker c70 exists to clear. Before c70 this exhausted the register allocator
   (one live vreg per case constant, ~350 of them). With the jump table it is
   O(1) and the ~350 case-constant vregs vanish.

   step(op) returns op*3+7 for op in [0..349], else -1 (default). run() sums
   step over a fixed program in a loop and folds to a known total; main() checks
   it plus a handful of boundary ops. gcc/clang exit 42 (cross-checked x86_64
   -O0/-O1 and aarch64 -O1 qemu). */

int step(int op) {
    switch (op) {
        case 0: return 7;
        case 1: return 10;
        case 2: return 13;
        case 3: return 16;
        case 4: return 19;
        case 5: return 22;
        case 6: return 25;
        case 7: return 28;
        case 8: return 31;
        case 9: return 34;
        case 10: return 37;
        case 11: return 40;
        case 12: return 43;
        case 13: return 46;
        case 14: return 49;
        case 15: return 52;
        case 16: return 55;
        case 17: return 58;
        case 18: return 61;
        case 19: return 64;
        case 20: return 67;
        case 21: return 70;
        case 22: return 73;
        case 23: return 76;
        case 24: return 79;
        case 25: return 82;
        case 26: return 85;
        case 27: return 88;
        case 28: return 91;
        case 29: return 94;
        case 30: return 97;
        case 31: return 100;
        case 32: return 103;
        case 33: return 106;
        case 34: return 109;
        case 35: return 112;
        case 36: return 115;
        case 37: return 118;
        case 38: return 121;
        case 39: return 124;
        case 40: return 127;
        case 41: return 130;
        case 42: return 133;
        case 43: return 136;
        case 44: return 139;
        case 45: return 142;
        case 46: return 145;
        case 47: return 148;
        case 48: return 151;
        case 49: return 154;
        case 50: return 157;
        case 51: return 160;
        case 52: return 163;
        case 53: return 166;
        case 54: return 169;
        case 55: return 172;
        case 56: return 175;
        case 57: return 178;
        case 58: return 181;
        case 59: return 184;
        case 60: return 187;
        case 61: return 190;
        case 62: return 193;
        case 63: return 196;
        case 64: return 199;
        case 65: return 202;
        case 66: return 205;
        case 67: return 208;
        case 68: return 211;
        case 69: return 214;
        case 70: return 217;
        case 71: return 220;
        case 72: return 223;
        case 73: return 226;
        case 74: return 229;
        case 75: return 232;
        case 76: return 235;
        case 77: return 238;
        case 78: return 241;
        case 79: return 244;
        case 80: return 247;
        case 81: return 250;
        case 82: return 253;
        case 83: return 256;
        case 84: return 259;
        case 85: return 262;
        case 86: return 265;
        case 87: return 268;
        case 88: return 271;
        case 89: return 274;
        case 90: return 277;
        case 91: return 280;
        case 92: return 283;
        case 93: return 286;
        case 94: return 289;
        case 95: return 292;
        case 96: return 295;
        case 97: return 298;
        case 98: return 301;
        case 99: return 304;
        case 100: return 307;
        case 101: return 310;
        case 102: return 313;
        case 103: return 316;
        case 104: return 319;
        case 105: return 322;
        case 106: return 325;
        case 107: return 328;
        case 108: return 331;
        case 109: return 334;
        case 110: return 337;
        case 111: return 340;
        case 112: return 343;
        case 113: return 346;
        case 114: return 349;
        case 115: return 352;
        case 116: return 355;
        case 117: return 358;
        case 118: return 361;
        case 119: return 364;
        case 120: return 367;
        case 121: return 370;
        case 122: return 373;
        case 123: return 376;
        case 124: return 379;
        case 125: return 382;
        case 126: return 385;
        case 127: return 388;
        case 128: return 391;
        case 129: return 394;
        case 130: return 397;
        case 131: return 400;
        case 132: return 403;
        case 133: return 406;
        case 134: return 409;
        case 135: return 412;
        case 136: return 415;
        case 137: return 418;
        case 138: return 421;
        case 139: return 424;
        case 140: return 427;
        case 141: return 430;
        case 142: return 433;
        case 143: return 436;
        case 144: return 439;
        case 145: return 442;
        case 146: return 445;
        case 147: return 448;
        case 148: return 451;
        case 149: return 454;
        case 150: return 457;
        case 151: return 460;
        case 152: return 463;
        case 153: return 466;
        case 154: return 469;
        case 155: return 472;
        case 156: return 475;
        case 157: return 478;
        case 158: return 481;
        case 159: return 484;
        case 160: return 487;
        case 161: return 490;
        case 162: return 493;
        case 163: return 496;
        case 164: return 499;
        case 165: return 502;
        case 166: return 505;
        case 167: return 508;
        case 168: return 511;
        case 169: return 514;
        case 170: return 517;
        case 171: return 520;
        case 172: return 523;
        case 173: return 526;
        case 174: return 529;
        case 175: return 532;
        case 176: return 535;
        case 177: return 538;
        case 178: return 541;
        case 179: return 544;
        case 180: return 547;
        case 181: return 550;
        case 182: return 553;
        case 183: return 556;
        case 184: return 559;
        case 185: return 562;
        case 186: return 565;
        case 187: return 568;
        case 188: return 571;
        case 189: return 574;
        case 190: return 577;
        case 191: return 580;
        case 192: return 583;
        case 193: return 586;
        case 194: return 589;
        case 195: return 592;
        case 196: return 595;
        case 197: return 598;
        case 198: return 601;
        case 199: return 604;
        case 200: return 607;
        case 201: return 610;
        case 202: return 613;
        case 203: return 616;
        case 204: return 619;
        case 205: return 622;
        case 206: return 625;
        case 207: return 628;
        case 208: return 631;
        case 209: return 634;
        case 210: return 637;
        case 211: return 640;
        case 212: return 643;
        case 213: return 646;
        case 214: return 649;
        case 215: return 652;
        case 216: return 655;
        case 217: return 658;
        case 218: return 661;
        case 219: return 664;
        case 220: return 667;
        case 221: return 670;
        case 222: return 673;
        case 223: return 676;
        case 224: return 679;
        case 225: return 682;
        case 226: return 685;
        case 227: return 688;
        case 228: return 691;
        case 229: return 694;
        case 230: return 697;
        case 231: return 700;
        case 232: return 703;
        case 233: return 706;
        case 234: return 709;
        case 235: return 712;
        case 236: return 715;
        case 237: return 718;
        case 238: return 721;
        case 239: return 724;
        case 240: return 727;
        case 241: return 730;
        case 242: return 733;
        case 243: return 736;
        case 244: return 739;
        case 245: return 742;
        case 246: return 745;
        case 247: return 748;
        case 248: return 751;
        case 249: return 754;
        case 250: return 757;
        case 251: return 760;
        case 252: return 763;
        case 253: return 766;
        case 254: return 769;
        case 255: return 772;
        case 256: return 775;
        case 257: return 778;
        case 258: return 781;
        case 259: return 784;
        case 260: return 787;
        case 261: return 790;
        case 262: return 793;
        case 263: return 796;
        case 264: return 799;
        case 265: return 802;
        case 266: return 805;
        case 267: return 808;
        case 268: return 811;
        case 269: return 814;
        case 270: return 817;
        case 271: return 820;
        case 272: return 823;
        case 273: return 826;
        case 274: return 829;
        case 275: return 832;
        case 276: return 835;
        case 277: return 838;
        case 278: return 841;
        case 279: return 844;
        case 280: return 847;
        case 281: return 850;
        case 282: return 853;
        case 283: return 856;
        case 284: return 859;
        case 285: return 862;
        case 286: return 865;
        case 287: return 868;
        case 288: return 871;
        case 289: return 874;
        case 290: return 877;
        case 291: return 880;
        case 292: return 883;
        case 293: return 886;
        case 294: return 889;
        case 295: return 892;
        case 296: return 895;
        case 297: return 898;
        case 298: return 901;
        case 299: return 904;
        case 300: return 907;
        case 301: return 910;
        case 302: return 913;
        case 303: return 916;
        case 304: return 919;
        case 305: return 922;
        case 306: return 925;
        case 307: return 928;
        case 308: return 931;
        case 309: return 934;
        case 310: return 937;
        case 311: return 940;
        case 312: return 943;
        case 313: return 946;
        case 314: return 949;
        case 315: return 952;
        case 316: return 955;
        case 317: return 958;
        case 318: return 961;
        case 319: return 964;
        case 320: return 967;
        case 321: return 970;
        case 322: return 973;
        case 323: return 976;
        case 324: return 979;
        case 325: return 982;
        case 326: return 985;
        case 327: return 988;
        case 328: return 991;
        case 329: return 994;
        case 330: return 997;
        case 331: return 1000;
        case 332: return 1003;
        case 333: return 1006;
        case 334: return 1009;
        case 335: return 1012;
        case 336: return 1015;
        case 337: return 1018;
        case 338: return 1021;
        case 339: return 1024;
        case 340: return 1027;
        case 341: return 1030;
        case 342: return 1033;
        case 343: return 1036;
        case 344: return 1039;
        case 345: return 1042;
        case 346: return 1045;
        case 347: return 1048;
        case 348: return 1051;
        case 349: return 1054;
        default: return -1;
    }
}

int run(void) {
    int prog[8];
    prog[0] = 0;      /* -> 7    */
    prog[1] = 1;      /* -> 10   */
    prog[2] = 100;    /* -> 307  */
    prog[3] = 200;    /* -> 607  */
    prog[4] = 349;    /* -> 1054 (last case) */
    prog[5] = 175;    /* -> 532  */
    prog[6] = 349;    /* -> 1054 */
    prog[7] = 0;      /* -> 7    */
    int total = 0;
    int i = 0;
    while (i < 8) {
        total = total + step(prog[i]);
        i = i + 1;
    }
    return total;   /* 7+10+307+607+1054+532+1054+7 = 3578 */
}

int main(void) {
    if (step(0)    != 7)    return 1;    /* first case  */
    if (step(349)  != 1054) return 2;    /* last case   */
    if (step(350)  != -1)   return 3;    /* above max -> default */
    if (step(-1)   != -1)   return 4;    /* below min -> default */
    if (step(175)  != 532)  return 5;    /* interior    */
    if (run()      != 3578) return 6;    /* hot-loop dispatch total */
    return 42;
}

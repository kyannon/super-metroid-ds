#!/usr/bin/env python3
"""Small 65816 disassembler for a LoROM SNES ROM (development aid).

    tools/dis65816.py ROM BANK:ADDR [COUNT] [m8|m16] [x8|x16]

Tracks REP/SEP for the accumulator / index sizes as it goes (straight-line)."""
import sys

OPS = {}
def op(code, name, mode): OPS[code] = (name, mode)
# mode keys: imp, imm_m, imm_x, imm8, dp, dpx, dpy, idp, idpx, idpy, idl, idly, sr, sry, abs, absx, absy, absl, abslx, iabs, iabsx, iabsl, rel, rell, mv
table = """
00 BRK imm8|01 ORA idpx|02 COP imm8|03 ORA sr|04 TSB dp|05 ORA dp|06 ASL dp|07 ORA idl|08 PHP imp|09 ORA imm_m|0A ASL imp|0B PHD imp|0C TSB abs|0D ORA abs|0E ASL abs|0F ORA absl
10 BPL rel|11 ORA idpy|12 ORA idp|13 ORA sry|14 TRB dp|15 ORA dpx|16 ASL dpx|17 ORA idly|18 CLC imp|19 ORA absy|1A INC imp|1B TCS imp|1C TRB abs|1D ORA absx|1E ASL absx|1F ORA abslx
20 JSR abs|21 AND idpx|22 JSL absl|23 AND sr|24 BIT dp|25 AND dp|26 ROL dp|27 AND idl|28 PLP imp|29 AND imm_m|2A ROL imp|2B PLD imp|2C BIT abs|2D AND abs|2E ROL abs|2F AND absl
30 BMI rel|31 AND idpy|32 AND idp|33 AND sry|34 BIT dpx|35 AND dpx|36 ROL dpx|37 AND idly|38 SEC imp|39 AND absy|3A DEC imp|3B TSC imp|3C BIT absx|3D AND absx|3E ROL absx|3F AND abslx
40 RTI imp|41 EOR idpx|42 WDM imm8|43 EOR sr|44 MVP mv|45 EOR dp|46 LSR dp|47 EOR idl|48 PHA imp|49 EOR imm_m|4A LSR imp|4B PHK imp|4C JMP abs|4D EOR abs|4E LSR abs|4F EOR absl
50 BVC rel|51 EOR idpy|52 EOR idp|53 EOR sry|54 MVN mv|55 EOR dpx|56 LSR dpx|57 EOR idly|58 CLI imp|59 EOR absy|5A PHY imp|5B TCD imp|5C JML absl|5D EOR absx|5E LSR absx|5F EOR abslx
60 RTS imp|61 ADC idpx|62 PER rell|63 ADC sr|64 STZ dp|65 ADC dp|66 ROR dp|67 ADC idl|68 PLA imp|69 ADC imm_m|6A ROR imp|6B RTL imp|6C JMP iabs|6D ADC abs|6E ROR abs|6F ADC absl
70 BVS rel|71 ADC idpy|72 ADC idp|73 ADC sry|74 STZ dpx|75 ADC dpx|76 ROR dpx|77 ADC idly|78 SEI imp|79 ADC absy|7A PLY imp|7B TDC imp|7C JMP iabsx|7D ADC absx|7E ROR absx|7F ADC abslx
80 BRA rel|81 STA idpx|82 BRL rell|83 STA sr|84 STY dp|85 STA dp|86 STX dp|87 STA idl|88 DEY imp|89 BIT imm_m|8A TXA imp|8B PHB imp|8C STY abs|8D STA abs|8E STX abs|8F STA absl
90 BCC rel|91 STA idpy|92 STA idp|93 STA sry|94 STY dpx|95 STA dpx|96 STX dpy|97 STA idly|98 TYA imp|99 STA absy|9A TXS imp|9B TXY imp|9C STZ abs|9D STA absx|9E STZ absx|9F STA abslx
A0 LDY imm_x|A1 LDA idpx|A2 LDX imm_x|A3 LDA sr|A4 LDY dp|A5 LDA dp|A6 LDX dp|A7 LDA idl|A8 TAY imp|A9 LDA imm_m|AA TAX imp|AB PLB imp|AC LDY abs|AD LDA abs|AE LDX abs|AF LDA absl
B0 BCS rel|B1 LDA idpy|B2 LDA idp|B3 LDA sry|B4 LDY dpx|B5 LDA dpx|B6 LDX dpy|B7 LDA idly|B8 CLV imp|B9 LDA absy|BA TSX imp|BB TYX imp|BC LDY absx|BD LDA absx|BE LDX absy|BF LDA abslx
C0 CPY imm_x|C1 CMP idpx|C2 REP imm8|C3 CMP sr|C4 CPY dp|C5 CMP dp|C6 DEC dp|C7 CMP idl|C8 INY imp|C9 CMP imm_m|CA DEX imp|CB WAI imp|CC CPY abs|CD CMP abs|CE DEC abs|CF CMP absl
D0 BNE rel|D1 CMP idpy|D2 CMP idp|D3 CMP sry|D4 PEI dp|D5 CMP dpx|D6 DEC dpx|D7 CMP idly|D8 CLD imp|D9 CMP absy|DA PHX imp|DB STP imp|DC JML iabsl|DD CMP absx|DE DEC absx|DF CMP abslx
E0 CPX imm_x|E1 SBC idpx|E2 SEP imm8|E3 SBC sr|E4 CPX dp|E5 SBC dp|E6 INC dp|E7 SBC idl|E8 INX imp|E9 SBC imm_m|EA NOP imp|EB XBA imp|EC CPX abs|ED SBC abs|EE INC abs|EF SBC absl
F0 BEQ rel|F1 SBC idpy|F2 SBC idp|F3 SBC sry|F4 PEA abs|F5 SBC dpx|F6 INC dpx|F7 SBC idly|F8 SED imp|F9 SBC absy|FA PLX imp|FB XCE imp|FC JSR iabsx|FD SBC absx|FE INC absx|FF SBC abslx
"""
for line in table.strip().split("\n"):
    for item in line.split("|"):
        c, n, m = item.split()
        op(int(c, 16), n, m)

def rom_off(bank, addr):
    return ((bank & 0x7F) << 15) | (addr & 0x7FFF)

def main():
    rom = open(sys.argv[1], "rb").read()
    if len(rom) % 0x8000 == 512:
        rom = rom[512:]
    b, a = sys.argv[2].split(":")
    bank, pc = int(b, 16), int(a, 16)
    count = int(sys.argv[3]) if len(sys.argv) > 3 else 40
    m8 = "m16" not in sys.argv
    x8 = "x16" not in sys.argv
    if "m8" not in sys.argv and "m16" not in sys.argv: m8 = False
    if "x8" not in sys.argv and "x16" not in sys.argv: x8 = False
    for _ in range(count):
        o = rom_off(bank, pc)
        code = rom[o]
        name, mode = OPS[code]
        size = {"imp": 1, "imm8": 2, "dp": 2, "dpx": 2, "dpy": 2, "idp": 2, "idpx": 2, "idpy": 2, "idl": 2, "idly": 2,
                "sr": 2, "sry": 2, "rel": 2, "abs": 3, "absx": 3, "absy": 3, "iabs": 3, "iabsx": 3, "rell": 3, "mv": 3,
                "absl": 4, "abslx": 4, "iabsl": 3}.get(mode)
        if mode == "imm_m": size = 2 if m8 else 3
        if mode == "imm_x": size = 2 if x8 else 3
        bs = rom[o:o + size]
        v = int.from_bytes(bs[1:], "little") if size > 1 else 0
        w = size - 1
        fmt = {"imp": "", "imm8": "#$%02X" % v, "imm_m": "#$%0*X" % (w * 2, v), "imm_x": "#$%0*X" % (w * 2, v),
               "dp": "$%02X" % v, "dpx": "$%02X,x" % v, "dpy": "$%02X,y" % v, "idp": "($%02X)" % v,
               "idpx": "($%02X,x)" % v, "idpy": "($%02X),y" % v, "idl": "[$%02X]" % v, "idly": "[$%02X],y" % v,
               "sr": "$%02X,s" % v, "sry": "($%02X,s),y" % v, "abs": "$%04X" % v, "absx": "$%04X,x" % v,
               "absy": "$%04X,y" % v, "iabs": "($%04X)" % v, "iabsx": "($%04X,x)" % v, "iabsl": "[$%04X]" % v,
               "absl": "$%06X" % v, "abslx": "$%06X,x" % v, "mv": ("$%02X,$%02X" % (bs[2], bs[1]) if size == 3 else "")}.get(mode, "")
        if mode == "rel":
            t = (pc + 2 + (v - 256 if v >= 128 else v)) & 0xFFFF
            fmt = "$%04X" % t
        if mode == "rell":
            t = (pc + 3 + (v - 65536 if v >= 32768 else v)) & 0xFFFF
            fmt = "$%04X" % t
        print("$%02X:%04X  %-12s %s %s" % (bank, pc, " ".join("%02X" % x for x in bs), name, fmt))
        if name == "REP":
            if v & 0x20: m8 = False
            if v & 0x10: x8 = False
        if name == "SEP":
            if v & 0x20: m8 = True
            if v & 0x10: x8 = True
        pc = (pc + size) & 0xFFFF

main()

#!/usr/bin/env python3
"""The car class table: classes for the Enhanced option CAR CLASSES: REORGANIZED and each car's
measured numbers for the vehicle screen (DESIGN.md point 8; runtime/classes.c).

Reads the class proposal (proposal.json: every car the game lists with Ant's fan folders, its
real vehicle, type, file stats, performance measured in the game and its proposed class; made in
the analysis of 2026-10-18 and approved by Ant) and writes the table the port builds in
(assets/car_classes.txt, gen/car_classes_txt.h): the classes in their order, then one line per
car. A car the table doesn't name keeps its own .vht letter; a car_classes.txt in the user
folder (the same lines) adds to or overrides the table.

  class <letter> <unlock> <NAME>    the classes in order; the first five are the championships'
                                    (x c p t w) and keep their letters; unlock "game" = as the
                                    game does it, "always" = always unlocked
  car <file> <letter> <top> <0-100> <0-50> <power>   # ...
                                    a car (its .idf name, lower case), its class, its top speed
                                    (km/h) and its 0-100 and 0-50 km/h times (s) measured on the
                                    flat Speed-Zone map, and its engine's power (the .vht's
                                    [engine] power); "-" where unknown (a car that never reached
                                    the speed, or one measured moving before GO, has no time;
                                    since runtime/fancars.c holds every car until GO, none
                                    does). The numbers may be left
                                    out (a user's own line): the vehicle screen then shows none.

usage: car_classes.py assets/car_classes_proposal.json > assets/car_classes.txt
(the proposal itself: work/classes in the 2026-10-18 session, copied there compact)"""
import json
import sys

# Ant's answers to the proposal (2026-10-18): the race classes are always unlocked; the
# championship classes and ODDITIES unlock as the game does it. The bonus cars (2026-10-19: in
# their type's classes, no BONUS class) stay locked until won wherever they are (classes.c).
UNLOCK = {'championship': 'game', 'other': 'game', 'race': 'always'}


def num(v, fmt):
    return '-' if v is None else fmt % v


def main(path):
    p = json.load(open(path))
    out = ['# 1NSANE native: car classes and measured numbers (DESIGN.md point 8). Made by',
           '# tools/car_classes.py from the class proposal Ant approved on 2026-10-18. A car not listed',
           '# keeps its own .vht letter. A car_classes.txt in the user folder (the same lines) adds or',
           '# overrides entries; its car lines may leave out the numbers.',
           '#',
           '# class <letter> <unlock> <NAME>: the classes in their order (the first five are the',
           '# championships\' and keep their letters); unlock "game" = as the game does it, "always" =',
           '# always unlocked. The bonus cars (won in the championships) stay locked until won, whatever',
           '# their class.', '']
    for c in p['classes']:
        out.append('class %s %-6s %s' % (c['letter'], UNLOCK[c['kind']], c['name']))
    out += ['', '# car <file> <letter> <top km/h> <0-100 s> <0-50 s> <power>   # the real vehicle; why', '']
    for car in sorted(p['cars'], key=lambda x: x['file']):
        cls = car['proposed_class']
        if not cls:
            raise SystemExit('%s has no class' % car['file'])
        m = car['measured']
        early = (m.get('start_kph') or 0) > 40      # moving before GO: its times mean nothing
        top = m.get('top_kph')
        t100 = None if early else m.get('t100_s')
        t50 = None if early else m.get('t50_s')
        power = car['stats'].get('engine_power')
        why = car['why'].replace('\n', ' ')
        out.append('car %-9s %s %4s %4s %4s %5s   # %s; %s' % (
            car['file'], cls['letter'], num(top, '%d'), num(t100, '%.1f'), num(t50, '%.1f'), num(power, '%.2f'),
            car['real_vehicle'], why))
    sys.stdout.write('\n'.join(out) + '\n')


if __name__ == '__main__':
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    main(sys.argv[1])

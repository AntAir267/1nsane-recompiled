/* Fan cars and maps (mods.c). kind: 0 = cars (Data\\Vehicles), 1 = maps (Data\\Maps). */
#pragma once
#include <stddef.h>

extern int mods_active;
void mods_init(void);
int mods_list(int kind, char names[][64], int max);               /* "name.idf" entries */
int mods_path(int kind, const char *name_idf, char *out, size_t n); /* host path, extracting once */
int mods_file(const char *rel, char *out, size_t n);  /* "Data/Vehicles/..." or "Data/Maps/...": host path */
const char *mods_download(int kind, const char *name);  /* a fan car's/map's download path, NULL if stock */

/* carnames.c: fan cars that share a name get a tag; long names shortened in Vehicle > Browse */
void carnames_tag(void);                    /* from the champdef hook: the car list is complete */
int carnames_fit(char *text);               /* a Browse row's text cut to the list's width; 1 if cut */

/* picker.c: jumps in the car and map lists (hooked with fan content) */
void picker_init(void);
const char *picker_hint(int controller);    /* the hint line while such a list has the focus, or NULL */

/* classes.c: the Enhanced option CAR CLASSES: ORIGINAL / REORGANIZED (with fan content) */
struct CPU;
void classes_init(void);                    /* from mods_init: the option and the table */
int classes_on(void);                       /* REORGANIZED this session */
void classes_letters(void);                 /* from the champdef hook: letters and order */
const char *classes_name(unsigned char letter);     /* a class's name, NULL if ORIGINAL or unnamed */
const char *classes_car_name(unsigned car);         /* the class name of a car list entry, or NULL */
void classes_unlock_begin(void);            /* before 0x46e690: the bonus cars as the game knows them */
void classes_unlock(void);                  /* after 0x46e690: the race classes' cars */
int classes_pick_prepare(struct CPU *c);    /* before 0x45e500: 1 if the list was changed for it */
void classes_pick_done(struct CPU *c);      /* after it: the list put back */
void classes_menus_init(void);              /* after opponents_menus_init, before prompts_init */
void classes_browse_sync(void);             /* Vehicle > Browse's position follows the chosen car */
void classes_browse_filter(int (*keep)(unsigned car));  /* STOCK CARS: the cars Browse lists, NULL all */

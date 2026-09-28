#pragma once

#define MAX_PROFILES 64
#define MAX_MODS 48
#define MAX_PATH_LEN 256
#define MAX_FILES 512

enum Presets
{
	PRESET_QUALITY,
	PRESET_BALANCED,
	PRESET_SWITCH,
	PRESET_COUNT
};

typedef struct
{
	char name[96];
	char iwad[MAX_PATH_LEN];
	int builtin;	// automatically created for an IWAD that was found
	int present;	// the IWAD exists
	int nmods;
	char mods[MAX_MODS][MAX_PATH_LEN];	// in load order (.deh/.bex go to -deh)
	int preset;
	int boost;
	int noloadscreen;
	int scale;	// render scale in percent
	int fpslimit;	// 0 = VSync 60, 30 = VSync every other refresh (locked 30), 35/40 = frame rate cap
	int glthread;	// GL driver on its own CPU core (Mesa glthread)
	int showfps;
	int skill;	// 0 = ask in game
	int monsters;	// index into monster options
	char warp[32];
	char pclass[64];
	char loadgame[MAX_PATH_LEN];
	int writelog;
	char extra[256];
	int lastplayed;	// counter for "recently played" order
} Profile;

typedef struct
{
	char name[MAX_PATH_LEN];
	double mb;
} FileEntry;

extern Profile profiles[MAX_PROFILES];
extern int nprofiles;
extern int cur_profile;

extern FileEntry iwad_files[MAX_FILES];
extern int n_iwad_files;
extern FileEntry pwad_files[MAX_FILES];
extern int n_pwad_files;

extern const char *const preset_names[PRESET_COUNT];
extern const char *const preset_desc[PRESET_COUNT];
extern const char *const monster_names[5];

int data_init(void);
void data_scan_files(void);
int data_save(void);
const char *data_iwad_title(const char *iwad);
// full path (relative to the launcher's directory) of a game file
void data_iwad_path(const char *iwad, char *out, int outlen);
double data_file_mb(const char *dir, const char *name);
int data_find_mod(const Profile *p, const char *name);
int data_add_profile(const char *name, const char *iwad, const Profile *copyfrom);
void data_delete_profile(int index);

// builds the engine's command line and schedules it; does not return on the Switch
void data_launch(int index);

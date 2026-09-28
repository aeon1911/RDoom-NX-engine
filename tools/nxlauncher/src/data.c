#include "data.h"
#include "platform.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

Profile profiles[MAX_PROFILES];
int nprofiles;
int cur_profile;

FileEntry iwad_files[MAX_FILES];
int n_iwad_files;
FileEntry pwad_files[MAX_FILES];
int n_pwad_files;

static int play_counter;
static int ini_version = 0;

const char *const preset_names[PRESET_COUNT] = { "Qualidade", "Equilibrado", "Switch (leve)" };
const char *const preset_desc[PRESET_COUNT] =
{
	"Tudo ligado, como no PC. Pode travar em cenas pesadas.",
	"Luzes leves; no Brutality: metade do sangue/cápsulas e barris de fogo simples.",
	"Sem luzes, efeitos mínimos, barris originais do Doom, monstros mais simples.",
};
const char *const monster_names[5] = { "Padrão", "Sem monstros", "Rápidos", "Renascem", "Rápidos e renascem" };

static const struct { const char *file, *title; } known_iwads[] =
{
	{ "doom_fusion.ipk3", "DOOM Fusion (WadFusion)" },
	{ "doom_complete.pk3", "Doom Complete (WadSmoosh)" },
	{ "doom.wad", "The Ultimate Doom" },
	{ "doom2.wad", "Doom II" },
	{ "doom2f.wad", "Doom II (Francês)" },
	{ "tnt.wad", "Final Doom: TNT Evilution" },
	{ "plutonia.wad", "Final Doom: The Plutonia Experiment" },
	{ "doom1.wad", "Doom (Shareware)" },
	{ "freedoom1.wad", "Freedoom: Phase 1" },
	{ "freedoom2.wad", "Freedoom: Phase 2" },
	{ "freedm.wad", "FreeDM" },
	{ "chex.wad", "Chex Quest" },
	{ "chex3.wad", "Chex Quest 3" },
	{ "heretic.wad", "Heretic" },
	{ "heretic1.wad", "Heretic (Shareware)" },
	{ "hexen.wad", "Hexen" },
	{ "hexdd.wad", "Hexen: Deathkings" },
	{ "strife1.wad", "Strife" },
	{ "sve.wad", "Strife: Veteran Edition" },
	{ "harm1.wad", "Harmony" },
	{ "hacx.wad", "HacX" },
	{ "rotwb.wad", "Rise of the Wool Ball" },
	{ "square1.pk3", "The Adventures of Square" },
	{ "blasphem.wad", "Blasphemer" },
	{ NULL, NULL }
};

// engine files that live next to gzdoom.nro and must not show up as games
static const char *const engine_files[] =
{
	"uzdoom.pk3", "gzdoom.pk3", "game_support.pk3", "brightmaps.pk3", "lights.pk3",
	"game_widescreen_gfx.pk3", "zd_extra.pk3", NULL
};

static int has_ext(const char *name, const char *const *exts)
{
	const char *d = strrchr(name, '.');
	if (!d) return 0;
	for (int i = 0; exts[i]; i++) if (!strcasecmp(d, exts[i])) return 1;
	return 0;
}

const char *data_iwad_title(const char *iwad)
{
	const char *base = strrchr(iwad, '/');
	base = base ? base + 1 : iwad;
	for (int i = 0; known_iwads[i].file; i++)
		if (!strcasecmp(base, known_iwads[i].file)) return known_iwads[i].title;
	return NULL;
}

double data_file_mb(const char *dir, const char *name)
{
	char path[600];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	struct stat st;
	if (stat(path, &st) == 0) return st.st_size / 1048576.0;
	return 0;
}

void data_iwad_path(const char *iwad, char *out, int outlen)
{
	snprintf(out, outlen, BASEDIR "/iwads/%s", iwad);
	struct stat st;
	if (stat(out, &st) == 0) return;
	snprintf(out, outlen, BASEDIR "/%s", iwad);
	if (stat(out, &st) == 0) return;
	snprintf(out, outlen, BASEDIR "/pwads/%s", iwad);
}

static int cmp_files(const void *a, const void *b)
{
	return strcasecmp(((const FileEntry *)a)->name, ((const FileEntry *)b)->name);
}

static void list_dir(const char *dir, const char *const *exts, int only_known, FileEntry *list, int *count)
{
	DIR *d = opendir(dir);
	if (!d) return;
	struct dirent *e;
	while ((e = readdir(d)) && *count < MAX_FILES)
	{
		if (e->d_name[0] == '.') continue;
		if (!has_ext(e->d_name, exts)) continue;
		char path[600];
		snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
		struct stat st;
		if (stat(path, &st) != 0 || S_ISDIR(st.st_mode)) continue;
		int skip = 0;
		for (int i = 0; engine_files[i]; i++) if (!strcasecmp(e->d_name, engine_files[i])) skip = 1;
		if (skip) continue;
		if (only_known && !data_iwad_title(e->d_name) && !has_ext(e->d_name, (const char *const[]){ ".ipk3", ".iwad", ".ipk7", NULL })) continue;
		// no duplicates
		for (int i = 0; i < *count; i++) if (!strcasecmp(list[i].name, e->d_name)) skip = 1;
		if (skip) continue;
		strncpy(list[*count].name, e->d_name, MAX_PATH_LEN - 1);
		list[*count].mb = st.st_size / 1048576.0;
		(*count)++;
	}
	closedir(d);
	qsort(list, *count, sizeof(FileEntry), cmp_files);
}

void data_scan_files(void)
{
	static const char *const iwad_exts[] = { ".wad", ".iwad", ".ipk3", ".ipk7", ".pk3", ".pk7", ".zip", NULL };
	static const char *const pwad_exts[] = { ".wad", ".pk3", ".pk7", ".zip", ".pke", ".ipk3", ".deh", ".bex", ".7z", NULL };
	n_iwad_files = 0;
	list_dir(BASEDIR "/iwads", iwad_exts, 0, iwad_files, &n_iwad_files);
	list_dir(BASEDIR, iwad_exts, 1, iwad_files, &n_iwad_files);
	n_pwad_files = 0;
	list_dir(BASEDIR "/pwads", pwad_exts, 0, pwad_files, &n_pwad_files);
}

static void profile_defaults(Profile *p)
{
	memset(p, 0, sizeof(*p));
	p->preset = PRESET_BALANCED;
	p->scale = 100;
	p->fpslimit = 0;
	p->glthread = 0;
	p->simplefx = 1;
}

static Profile *find_profile(const char *name)
{
	for (int i = 0; i < nprofiles; i++)
		if (!strcasecmp(profiles[i].name, name)) return &profiles[i];
	return NULL;
}

static int iwad_present(const char *iwad)
{
	for (int i = 0; i < n_iwad_files; i++) if (!strcasecmp(iwad_files[i].name, iwad)) return 1;
	return 0;
}

int data_find_mod(const Profile *p, const char *name)
{
	for (int i = 0; i < p->nmods; i++) if (!strcasecmp(p->mods[i], name)) return i;
	return -1;
}

int data_is_pb(const Profile *p)
{
	for (int i = 0; i < p->nmods; i++)
	{
		char low[MAX_PATH_LEN];
		int j;
		for (j = 0; p->mods[i][j] && j < MAX_PATH_LEN - 1; j++) low[j] = (char)tolower((unsigned char)p->mods[i][j]);
		low[j] = 0;
		if (strstr(low, "brutal") || !strncmp(low, "pb_", 3) || !strncmp(low, "pb-", 3) || strstr(low, "project_b") || strstr(low, "projectb")) return 1;
	}
	return 0;
}

//==========================================================================
// ini file
//==========================================================================

static char last_profile_name[96];

static void trim(char *s)
{
	char *p = s;
	while (*p && isspace((unsigned char)*p)) p++;
	if (p != s) memmove(s, p, strlen(p) + 1);
	size_t n = strlen(s);
	while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = 0;
}

static void load_ini(void)
{
	FILE *f = fopen(BASEDIR "/launcher.ini", "r");
	if (!f) return;
	char line[1024];
	Profile *p = NULL;
	int in_launcher = 0;
	while (fgets(line, sizeof(line), f))
	{
		trim(line);
		if (!line[0] || line[0] == ';' || line[0] == '#') continue;
		if (line[0] == '[')
		{
			char *e = strrchr(line, ']');
			if (e) *e = 0;
			const char *name = line + 1;
			in_launcher = !strcmp(name, "@Launcher");
			p = NULL;
			if (in_launcher) continue;
			p = find_profile(name);
			if (!p && nprofiles < MAX_PROFILES)
			{
				p = &profiles[nprofiles++];
				profile_defaults(p);
				strncpy(p->name, name, sizeof(p->name) - 1);
				p->builtin = 0;
			}
			else if (p)
			{
				// the saved mod list replaces the defaults
				p->nmods = 0;
			}
			continue;
		}
		char *eq = strchr(line, '=');
		if (!eq) continue;
		*eq = 0;
		char *key = line, *val = eq + 1;
		trim(key);
		trim(val);
		if (in_launcher)
		{
			if (!strcasecmp(key, "LastProfile")) strncpy(last_profile_name, val, sizeof(last_profile_name) - 1);
			else if (!strcasecmp(key, "Version")) ini_version = atoi(val);
			else if (!strcasecmp(key, "PlayCounter")) play_counter = atoi(val);
			continue;
		}
		if (!p) continue;
		if (!strcasecmp(key, "IWAD")) strncpy(p->iwad, val, MAX_PATH_LEN - 1);
		else if (!strcasecmp(key, "File") || !strcasecmp(key, "DEHFile") || !strcasecmp(key, "Mod"))
		{
			if (val[0] && p->nmods < MAX_MODS && data_find_mod(p, val) < 0) strncpy(p->mods[p->nmods++], val, MAX_PATH_LEN - 1);
		}
		else if (!strcasecmp(key, "Preset")) p->preset = atoi(val);
		else if (!strcasecmp(key, "Boost")) p->boost = 0;	// option removed (no clock changes)
		else if (!strcasecmp(key, "NoLoadScreen")) p->noloadscreen = atoi(val);
		else if (!strcasecmp(key, "RenderScale")) p->scale = atoi(val);
		else if (!strcasecmp(key, "FPSLimit")) p->fpslimit = atoi(val);
		else if (!strcasecmp(key, "GLThread")) p->glthread = atoi(val);
		else if (!strcasecmp(key, "SimpleShotFX")) p->simplefx = atoi(val);
		else if (!strcasecmp(key, "ShowFPS")) p->showfps = atoi(val);
		else if (!strcasecmp(key, "Skill")) p->skill = atoi(val);
		else if (!strcasecmp(key, "Monsters"))
		{
			// old launcher stored the command line flag combination
			int v = atoi(val);
			p->monsters = v == 1 ? 1 : v == 2 ? 2 : v == 4 ? 3 : v == 6 ? 4 : 0;
		}
		else if (!strcasecmp(key, "MonsterMode")) p->monsters = atoi(val);
		else if (!strcasecmp(key, "StartMap")) strncpy(p->warp, val, sizeof(p->warp) - 1);
		else if (!strcasecmp(key, "PlayerClass")) strncpy(p->pclass, val, sizeof(p->pclass) - 1);
		else if (!strcasecmp(key, "LoadGame")) strncpy(p->loadgame, val, MAX_PATH_LEN - 1);
		else if (!strcasecmp(key, "WriteLog")) p->writelog = atoi(val);
		else if (!strcasecmp(key, "ExtraArgs")) strncpy(p->extra, val, sizeof(p->extra) - 1);
		else if (!strcasecmp(key, "LastPlayed")) p->lastplayed = atoi(val);
	}
	fclose(f);
}

int data_save(void)
{
	mkdir(BASEDIR, 0755);
	FILE *f = fopen(BASEDIR "/launcher.ini.tmp", "w");
	if (!f) return -1;
	fprintf(f, "; UZDoom Switch launcher profiles\n\n[@Launcher]\nVersion = 6\n");
	if (cur_profile >= 0 && cur_profile < nprofiles) fprintf(f, "LastProfile = %s\n", profiles[cur_profile].name);
	fprintf(f, "PlayCounter = %d\n\n", play_counter);
	for (int i = 0; i < nprofiles; i++)
	{
		Profile *p = &profiles[i];
		fprintf(f, "[%s]\n", p->name);
		fprintf(f, "IWAD = %s\n", p->iwad);
		for (int m = 0; m < p->nmods; m++) fprintf(f, "Mod = %s\n", p->mods[m]);
		fprintf(f, "Preset = %d\n", p->preset);
		fprintf(f, "Boost = %d\n", p->boost);
		if (p->noloadscreen) fprintf(f, "NoLoadScreen = %d\n", p->noloadscreen);
		fprintf(f, "RenderScale = %d\n", p->scale);
		fprintf(f, "FPSLimit = %d\n", p->fpslimit);
		fprintf(f, "GLThread = %d\n", p->glthread);
		fprintf(f, "SimpleShotFX = %d\n", p->simplefx);
		fprintf(f, "ShowFPS = %d\n", p->showfps);
		fprintf(f, "Skill = %d\n", p->skill);
		fprintf(f, "MonsterMode = %d\n", p->monsters);
		if (p->warp[0]) fprintf(f, "StartMap = %s\n", p->warp);
		if (p->pclass[0]) fprintf(f, "PlayerClass = %s\n", p->pclass);
		if (p->loadgame[0]) fprintf(f, "LoadGame = %s\n", p->loadgame);
		fprintf(f, "WriteLog = %d\n", p->writelog);
		if (p->extra[0]) fprintf(f, "ExtraArgs = %s\n", p->extra);
		if (p->lastplayed) fprintf(f, "LastPlayed = %d\n", p->lastplayed);
		fprintf(f, "\n");
	}
	fclose(f);
	remove(BASEDIR "/launcher.ini");
	return rename(BASEDIR "/launcher.ini.tmp", BASEDIR "/launcher.ini");
}

int data_init(void)
{
	mkdir(BASEDIR, 0755);
	mkdir(BASEDIR "/iwads", 0755);
	mkdir(BASEDIR "/pwads", 0755);
	mkdir(TMPDIR, 0755);
	data_scan_files();

	// one profile per game file that was found
	nprofiles = 0;
	for (int i = 0; known_iwads[i].file; i++)
	{
		if (!iwad_present(known_iwads[i].file)) continue;
		Profile *p = &profiles[nprofiles++];
		profile_defaults(p);
		strncpy(p->name, known_iwads[i].title, sizeof(p->name) - 1);
		strncpy(p->iwad, known_iwads[i].file, MAX_PATH_LEN - 1);
		p->builtin = 1;
	}
	for (int i = 0; i < n_iwad_files && nprofiles < MAX_PROFILES; i++)
	{
		if (data_iwad_title(iwad_files[i].name)) continue;
		Profile *p = &profiles[nprofiles++];
		profile_defaults(p);
		// file name without extension
		strncpy(p->name, iwad_files[i].name, sizeof(p->name) - 1);
		char *dot = strrchr(p->name, '.');
		if (dot) *dot = 0;
		strncpy(p->iwad, iwad_files[i].name, MAX_PATH_LEN - 1);
		p->builtin = 1;
	}

	ini_version = 0;
	load_ini();

	// nx3.2: Project Brutality profiles move to the lightest preset once
	// (the heavy fights were far too slow with the others)
	if (ini_version < 3)
	{
		for (int i = 0; i < nprofiles; i++)
			if (data_is_pb(&profiles[i])) profiles[i].preset = PRESET_SWITCH;
	}
	// nx3.3: Brutality profiles get a steady 30 FPS cap once
	if (ini_version < 4)
	{
		for (int i = 0; i < nprofiles; i++)
			if (data_is_pb(&profiles[i])) profiles[i].fpslimit = 30;
	}
	// nx3.5: GL driver thread on for everybody, Brutality locked at 30 (VSync every other refresh)
	if (ini_version < 5)
	{
		for (int i = 0; i < nprofiles; i++)
		{
			profiles[i].glthread = 1;
			if (data_is_pb(&profiles[i])) profiles[i].fpslimit = 30;
		}
	}
	// nx3.7: plain shot effects for everybody (only used with Brutality)
	if (ini_version < 6)
	{
		// the GL driver thread froze the game on the console (nx3.6): off until it's proven
		for (int i = 0; i < nprofiles; i++) { profiles[i].simplefx = 1; profiles[i].glthread = 0; }
	}

	// drop entries without a game file (keep custom ones so that nothing gets lost)
	for (int i = 0; i < nprofiles; i++)
	{
		profiles[i].present = iwad_present(profiles[i].iwad);
		if (profiles[i].preset < 0 || profiles[i].preset >= PRESET_COUNT) profiles[i].preset = PRESET_BALANCED;
		if (profiles[i].scale < 50 || profiles[i].scale > 100) profiles[i].scale = 100;
		if (profiles[i].fpslimit != 30 && profiles[i].fpslimit != 35 && profiles[i].fpslimit != 40) profiles[i].fpslimit = 0;
		profiles[i].glthread = profiles[i].glthread ? 1 : 0;
		profiles[i].simplefx = profiles[i].simplefx ? 1 : 0;
		if (profiles[i].monsters < 0 || profiles[i].monsters > 4) profiles[i].monsters = 0;
		if (profiles[i].skill < 0 || profiles[i].skill > 5) profiles[i].skill = 0;
	}

	cur_profile = -1;
	for (int i = 0; i < nprofiles; i++)
		if (profiles[i].present && !strcasecmp(profiles[i].name, last_profile_name)) cur_profile = i;
	if (cur_profile < 0)
		for (int i = 0; i < nprofiles; i++) if (profiles[i].present) { cur_profile = i; break; }
	return cur_profile >= 0 ? 0 : -1;
}

int data_add_profile(const char *name, const char *iwad, const Profile *copyfrom)
{
	if (nprofiles >= MAX_PROFILES || !name[0]) return -1;
	char uname[96];
	strncpy(uname, name, sizeof(uname) - 1);
	uname[sizeof(uname) - 1] = 0;
	// unique name
	for (int n = 2; find_profile(uname) && n < 100; n++) snprintf(uname, sizeof(uname), "%.80s (%d)", name, n);
	Profile *p = &profiles[nprofiles];
	if (copyfrom) *p = *copyfrom;
	else profile_defaults(p);
	strncpy(p->name, uname, sizeof(p->name) - 1);
	p->name[sizeof(p->name) - 1] = 0;
	strncpy(p->iwad, iwad, MAX_PATH_LEN - 1);
	p->builtin = 0;
	p->present = iwad_present(iwad);
	p->lastplayed = 0;
	return nprofiles++;
}

void data_delete_profile(int index)
{
	if (index < 0 || index >= nprofiles) return;
	memmove(&profiles[index], &profiles[index + 1], (nprofiles - index - 1) * sizeof(Profile));
	nprofiles--;
	if (cur_profile >= nprofiles) cur_profile = nprofiles - 1;
}

//==========================================================================
// starting the engine
//==========================================================================

static char *argp;
static int argleft;

static void arg(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(argp, argleft, fmt, ap);
	va_end(ap);
	if (n > 0 && n < argleft) { argp += n; argleft -= n; }
}

static int is_deh(const char *n)
{
	const char *d = strrchr(n, '.');
	return d && (!strcasecmp(d, ".deh") || !strcasecmp(d, ".bex"));
}

// snd_channels/snd_efx are applied by the engine before its sound system starts
static const char *const engine_presets[PRESET_COUNT] =
{
	"+set r_dynlights 1 +set gles_max_lights_per_surface 16 +set r_maxparticles 4000 +set cl_maxdecals 1024 +set gl_light_particles 1 "
	"+set r_actorspriteshadow 1 +set r_actorspriteshadowdist 1500 +set gl_texture_filter_anisotropic 8 +set gl_sort_textures 0 +set snd_channels 128 +set snd_efx 1 "
	"+set nx_effect_cull 0 +set nx_effect_minalpha 0 +set nx_fx_thin none +set nx_fx_far 0 ",

	"+set r_dynlights 1 +set gles_max_lights_per_surface 6 +set r_maxparticles 600 +set cl_maxdecals 200 +set gl_light_particles 0 "
	"+set r_actorspriteshadow 1 +set r_actorspriteshadowdist 800 +set gl_texture_filter_anisotropic 4 +set gl_sort_textures 1 +set snd_channels 64 +set snd_efx 0 "
	"+set nx_effect_cull 4000 +set nx_effect_minalpha 0.06 +set nx_fx_thin none +set nx_fx_far 0 ",

	"+set r_dynlights 0 +set gles_max_lights_per_surface 2 +set r_maxparticles 150 +set cl_maxdecals 64 +set gl_light_particles 0 "
	"+set r_actorspriteshadow 1 +set r_actorspriteshadowdist 600 +set gl_texture_filter_anisotropic 2 +set gl_sort_textures 1 +set snd_channels 40 +set snd_efx 0 "
	"+set nx_effect_cull 2500 +set nx_effect_minalpha 0.12 +set nx_fx_thin none +set nx_fx_far 0 ",
};

// Project Brutality's own options (only passed when PB is in the mod list).
// Most of PB's cost in fights is the number of effect actors (blood, gibs,
// casings, smoke) that all move and get drawn every tic.
static const char *const pb_presets[PRESET_COUNT] =
{
	"+set PB_tracerlight 1 +set pb_performance_fire 0 +set pb_simplemonsterbehavior 0 +set pb_disable_decorations 0 +set pb_decorative_flame_barrels 0 "
	"+set nashgore_maxgore 1024 +set nashgore_maxcasings 50 +set nashgore_bloodmult 1.0 +set nashgore_gibamount 255 "
	"+set nashgore_footprints 1 +set nashgore_bloodspurt 1 +set nashgore_corpseblood 1 +set nashgore_splat 1 "
	"+set pb_hidebloodmist 0 +set pb_cheapcasings 1 +set pb_dasheffect 1 +set pb_lowflashesmode 0 +set pb_showblooddrops 1 +set pb_showglasscracks 1 "
	"+set pb_gunsmoketype 0 ",

	// weapon smoke off (PB's own "Gun Smoke Type: Off" plus the barrel heat,
	// casing and enemy muzzle smoke that option doesn't cover); nothing else is thinned
	"+set PB_tracerlight 0 +set pb_performance_fire 1 +set pb_simplemonsterbehavior 0 +set pb_disable_decorations 0 +set pb_decorative_flame_barrels 1 "
	"+set nashgore_maxgore 128 +set nashgore_maxcasings 16 +set nashgore_bloodmult 0.6 +set nashgore_gibamount 128 "
	"+set nashgore_footprints 0 +set nashgore_bloodspurt 1 +set nashgore_corpseblood 0 +set nashgore_splat 1 "
	"+set pb_hidebloodmist 1 +set pb_cheapcasings 1 +set pb_dasheffect 1 +set pb_lowflashesmode 0 +set pb_showblooddrops 1 +set pb_showglasscracks 1 "
	"+set pb_gunsmoketype 2 ",

	"+set PB_tracerlight 0 +set pb_performance_fire 1 +set pb_simplemonsterbehavior 1 +set pb_disable_decorations 1 +set pb_decorative_flame_barrels 1 "
	"+set nashgore_maxgore 40 +set nashgore_maxcasings 6 +set nashgore_bloodmult 0.3 +set nashgore_gibamount 48 "
	"+set nashgore_footprints 0 +set nashgore_bloodspurt 0 +set nashgore_corpseblood 0 +set nashgore_splat 0 "
	"+set pb_hidebloodmist 1 +set pb_cheapcasings 1 +set pb_dasheffect 0 +set pb_lowflashesmode 1 +set pb_showblooddrops 0 +set pb_showglasscracks 0 "
	"+set pb_huddynamics 0 +set pb_execution_box 0 "
	"+set pb_gunsmoketype 2 ",
};

void data_launch(int index)
{
	Profile *p = &profiles[index];
	static char args[8192];
	static char exe[640];
	static char full[8192 + 700];

	p->lastplayed = ++play_counter;
	cur_profile = index;
	data_save();

	// the engine's loading screen shows the profile name
	FILE *f = fopen(TMPDIR "/loadinfo.txt", "w");
	if (f)
	{
		fprintf(f, "%s\n", p->name);
		fclose(f);
	}

	argp = args;
	argleft = sizeof(args);
	args[0] = 0;
	arg("-iwad \"%s\" ", p->iwad);

	int nfiles = 0, ndeh = 0;
	for (int i = 0; i < p->nmods; i++) is_deh(p->mods[i]) ? ndeh++ : nfiles++;
	if (nfiles)
	{
		arg("-file");
		for (int i = 0; i < p->nmods; i++) if (!is_deh(p->mods[i])) arg(" \"%s\"", p->mods[i]);
		arg(" ");
	}
	if (ndeh)
	{
		arg("-deh");
		for (int i = 0; i < p->nmods; i++) if (is_deh(p->mods[i])) arg(" \"%s\"", p->mods[i]);
		arg(" ");
	}
	if (p->skill > 0) arg("-skill %d ", p->skill);
	switch (p->monsters)
	{
	case 1: arg("-nomonsters "); break;
	case 2: arg("-fast "); break;
	case 3: arg("-respawn "); break;
	case 4: arg("-fast -respawn "); break;
	}
	if (p->loadgame[0]) arg("-loadgame \"save/%s\" ", p->loadgame);
	// no CPU/GPU clock changes of any kind: -nxboost is never passed
	if (p->noloadscreen) arg("-nxnoload ");

	arg("%s", engine_presets[p->preset]);
	if (data_is_pb(p))
	{
		arg("%s", pb_presets[p->preset]);
		// purely visual effect actors that are dropped (nx_fx_thin, class = share kept)
		char fx[1024] = "";
		if (p->preset != PRESET_QUALITY)
			// weapon smoke: barrel heat, casing and enemy muzzle smoke (PB's "Gun Smoke Type: Off" misses those)
			strcat(fx, "PB_GunFireSmoke=0,GunFireSmoke=0,NewCasingSmoke=0,");
		if (p->simplefx)
			// shot effects like in the original game: no detailed bullet impacts, debris, sparks,
			// ricochets, ejected casings or extra muzzle flashes (bullets, damage and blood stay)
			strcat(fx, "PB_BaseBulletImpact=0,PB_AnimatedSmokeThinker=0,SparkX=0,HitSpark=0,RicoChet=0,"
				"PB_CasingBase=0,RifleCaseSpawn=0,Mp40CaseSpawn=0,SMGCasingSpawner=0,MastermindCaseSpawn=0,ShotgunWad=0,"
				"OrangeLensFlareAlt=0,PB_MuzzleSpark=0,MarineMuzzle1=0,");
		size_t n = strlen(fx);
		if (n > 0) { fx[n - 1] = 0; arg("+set nx_fx_thin %s ", fx); }
	}
	arg("+set vid_scalefactor %d.%02d ", p->scale / 100, p->scale % 100);
	// 30 = VSync on every other refresh of the 60 Hz screen: a real lock with
	// even frame pacing. 35/40 are plain caps without VSync.
	if (p->fpslimit == 30) arg("+set vid_vsync 1 +set nx_swapinterval 2 +set cl_capfps 0 +set vid_maxfps 0 ");
	else if (p->fpslimit == 35) arg("+set vid_vsync 0 +set nx_swapinterval 1 +set cl_capfps 1 +set vid_maxfps 0 ");
	else if (p->fpslimit > 0) arg("+set vid_vsync 0 +set nx_swapinterval 1 +set cl_capfps 0 +set vid_maxfps %d ", p->fpslimit);
	else arg("+set vid_vsync 1 +set nx_swapinterval 1 +set cl_capfps 0 +set vid_maxfps 0 ");
	arg("+set nx_glthread %d +set r_ticstability 0 ", p->glthread ? 1 : 0);
	arg("+set vid_fps %d ", p->showfps ? 1 : 0);
	if (p->warp[0]) arg("+map %s ", p->warp);
	if (p->pclass[0]) arg("+playerclass \"%s\" ", p->pclass);
	if (p->writelog) arg("+logfile gzdoom.log ");
	if (p->extra[0]) arg("%s ", p->extra);

	snprintf(exe, sizeof(exe), "%s/%s/gzdoom.nro", plat_cwd(), BASEDIR);
	snprintf(full, sizeof(full), "%s %s", exe, args);
	plat_set_next_load(exe, full);
}

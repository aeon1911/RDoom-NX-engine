/*
** rdoom_gpubench.cpp
** On-console GPU benchmark (Switch).
**
** rd_gpubench 1 (e.g. "rd_gpubench=1" in rdoom_debug.txt) waits until a map
** has been on screen for a few seconds, freezes the level and renders the
** same spot with one setting changed at a time, looking in four directions.
** The GPU time of every frame comes from timestamps (nx_perf), so the table
** in logs/gpu-bench.txt shows what each option really costs on the Tegra X1,
** in handheld or docked mode, independent of vsync.
**
** Copyright 2026 RDoom contributors. GPLv3 (same as UZDoom).
*/

#include "rdoom_gpubench.h"

#ifdef __SWITCH__

#include <stdio.h>
#include <string>
#include <vector>
#include <switch.h>
#include "c_cvars.h"
#include "d_player.h"
#include "g_levellocals.h"
#include "gamestate.h"
#include "nx_perf.h"
#include "printf.h"
#include "v_video.h"
#include "common/platform/posix/nx/rdoom_nx.h"
#include "rdoom_version.h"

CVAR(Int, rd_gpubench, 0, 0)

extern int nx_levelframes;

namespace
{
	struct Setting { const char *name; const char *value; };
	struct Config { const char *label; std::vector<Setting> settings; };

	const std::vector<Config> configs = {
		{ "base (perfil atual)", {} },
		{ "sem luzes dinamicas", { { "r_dynlights", "0" } } },
		{ "escala 0.75", { { "vid_scalefactor", "0.75" } } },
		{ "escala 0.5", { { "vid_scalefactor", "0.5" } } },
		{ "cena RGB10A2", { { "rd_vk_scenefmt", "1" } } },
		{ "cena RGBA8", { { "rd_vk_scenefmt", "2" } } },
		{ "ZCULL ligado", { { "rd_vk_zcull", "1" } } },
		{ "copia da cena (sem troca)", { { "rd_vk_sceneswap", "0" } } },
		{ "RGBA8 + ZCULL", { { "rd_vk_scenefmt", "2" }, { "rd_vk_zcull", "1" } } },
		{ "base de novo", {} },
	};
	const int Angles = 4, SettleFrames = 30, MeasureFrames = 60;

	struct BenchResult { double frame = 0, scene = 0, post = 0, end = 0, cpu = 0; int gpu = 0, cpuframes = 0; };

	bool running = false;
	int config = 0, angle = 0, frameInStep = 0;
	DAngle baseYaw;
	std::vector<std::pair<std::string, std::string>> saved;	// cvar, original value
	std::vector<BenchResult> results;

	FBaseCVar *Find(const char *name) { return FindCVar(name, nullptr); }

	void Set(const char *name, const char *value)
	{
		if (FBaseCVar *cv = Find(name))
		{
			UCVarValue v;
			v.String = value;
			cv->SetGenericRep(v, CVAR_String);
		}
	}

	void ApplyConfig(int c)
	{
		for (auto &s : saved)
			Set(s.first.c_str(), s.second.c_str());
		for (auto &s : configs[c].settings)
			Set(s.name, s.value);
	}

	void Look(player_t *p)
	{
		p->mo->Angles.Yaw = baseYaw + DAngle::fromDeg(90. * angle);
		p->mo->PrevAngles = p->mo->Angles;
		p->mo->Vel.Zero();
	}

	void Finish(bool complete)
	{
		for (auto &s : saved)
			Set(s.first.c_str(), s.second.c_str());
		if (primaryLevel)
			primaryLevel->frozenstate &= ~1;
		running = false;
		rd_gpubench = 0;

		FILE *f = fopen(RDOOM_SD_ROOT "/logs/gpu-bench.txt", "w");
		if (f)
		{
			fprintf(f, "RDoom GPU benchmark (%s, build %s)\n", complete ? "completo" : "interrompido", RDOOM_GIT_DESCRIBE);
			fprintf(f, "modo: %s | mapa: %s | tela %dx%d | %d direcoes x %d frames por configuracao\n",
				appletGetOperationMode() == AppletOperationMode_Console ? "dock" : "portatil",
				primaryLevel ? primaryLevel->MapName.GetChars() : "?", screen->GetWidth(), screen->GetHeight(), Angles, MeasureFrames);
			fprintf(f, "tempos de GPU em ms por frame (16.7 ms = 60 fps); cpu = frame inteiro visto pela CPU\n\n");
			fprintf(f, "%-28s %8s %8s %8s %8s %8s %8s\n", "configuracao", "gpu", "cena3D", "posproc", "2D", "vs base", "cpu");
			double base = 0;
			for (size_t c = 0; c < results.size(); c++)
			{
				const BenchResult &r = results[c];
				if (r.gpu == 0)
				{
					fprintf(f, "%-28s %8s\n", configs[c].label, "sem dados");
					continue;
				}
				double g = r.gpu;
				if (c == 0)
					base = r.frame / g;
				fprintf(f, "%-28s %8.2f %8.2f %8.2f %8.2f %+7.0f%% %8.2f\n", configs[c].label, r.frame / g, r.scene / g, r.post / g, r.end / g,
					base > 0 ? (r.frame / g / base - 1) * 100 : 0.0, r.cpuframes ? r.cpu / r.cpuframes : 0.0);
			}
			fclose(f);
		}
		Printf("Benchmark de GPU %s: logs/gpu-bench.txt\n", complete ? "concluido" : "interrompido");
		RDoom_SessionSet("gpu_bench", complete ? "complete (logs/gpu-bench.txt)" : "interrupted");
	}
}

void RDoom_GpuBenchFrame()
{
	player_t *p = primaryLevel ? primaryLevel->GetConsolePlayer() : nullptr;
	const bool inLevel = gamestate == GS_LEVEL && p && p->mo;

	if (!running)
	{
		// start once the map has been on screen for a while (textures and
		// pipelines of the first view are loaded by then)
		if (rd_gpubench <= 0 || !inLevel || nx_levelframes < 150)
			return;
		running = true;
		config = angle = frameInStep = 0;
		results.assign(configs.size(), BenchResult());
		saved.clear();
		for (auto &c : configs)
			for (auto &s : c.settings)
			{
				bool known = false;
				for (auto &k : saved)
					known |= k.first == s.name;
				FBaseCVar *cv = Find(s.name);
				if (!known && cv)
					saved.emplace_back(s.name, cv->GetHumanString());
			}
		baseYaw = p->mo->Angles.Yaw;
		primaryLevel->frozenstate |= 1;	// monsters, projectiles and the player stand still
		Printf("Benchmark de GPU: nao mexa no controle (cerca de 2 minutos)\n");
		ApplyConfig(0);
	}

	if (!inLevel || rd_gpubench <= 0)
	{
		Finish(false);
		return;
	}

	Look(p);
	if (frameInStep >= SettleFrames)
	{
		BenchResult &r = results[config];
		r.gpu += nxperf.gpuframes;
		r.frame += nxperf.gpu_frame_ms;
		r.scene += nxperf.gpu_scene_ms;
		r.post += nxperf.gpu_post_ms;
		r.end += nxperf.gpu_end_ms;
		r.cpu += NXPerf_LastFrameMS();
		r.cpuframes++;
	}
	if (++frameInStep < SettleFrames + MeasureFrames)
		return;

	frameInStep = 0;
	if (++angle < Angles)
		return;
	angle = 0;
	if (++config < (int)configs.size())
	{
		ApplyConfig(config);
		return;
	}
	Finish(true);
}

#else

void RDoom_GpuBenchFrame()
{
}

#endif

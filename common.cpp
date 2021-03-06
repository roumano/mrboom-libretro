#include <unistd.h>
#include <string.h>
#include <file/file_path.h>
#include "mrboom.h"
#include "common.hpp"
#include "MrboomHelper.hpp"
#include "BotTree.hpp"
#include <unistd.h>
#include <string.h>
#ifdef __LIBSDL2__
#define LOAD_FROM_FILES
#endif

#ifdef LOAD_FROM_FILES
#include "streams/file_stream.h"
#include <minizip/unzip.h>
#endif

#define SOUND_VOLUME 2
#define NB_WAV                16
#define NB_VOICES             28
#define keyboardCodeOffset    32
#define keyboardReturnKey     28
#define keyboardExitKey       1
#define keyboardDataSize      8
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define keyboardExtraSelectStartKeysSize 2
#define offsetExtraKeys keyboardDataSize*nb_dyna+keyboardCodeOffset

#pragma GCC diagnostic ignored "-Woverlength-strings"
#pragma GCC diagnostic ignored "-Warray-bounds"

#ifdef __LIBRETRO__
#include "retro_data.h"
#include "retro.hpp"

#ifdef LOAD_FROM_FILES
#include <audio/audio_mix.h>
static audio_chunk_t *wave[NB_WAV];
#endif

static size_t frames_left[NB_WAV];


#ifndef INT16_MAX
#define INT16_MAX   0x7fff
#endif

#ifndef INT16_MIN
#define INT16_MIN   (-INT16_MAX - 1)
#endif

#define CLAMP_I16(x) (x > INT16_MAX ? INT16_MAX : x < INT16_MIN ? INT16_MIN : x)
#endif

#ifdef __LIBSDL2__
#include "sdl2_data.h"
#define LOAD_FROM_FILES
#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
static Mix_Chunk * wave[NB_WAV];
#define NB_CHIPTUNES              8
static Mix_Music *musics[NB_CHIPTUNES];
static int musics_index = 0;
bool music=true;
const char * musics_filenames[NB_CHIPTUNES] = {
	"deadfeelings.XM", // Carter (for menu + replay)
	"chiptune.MOD", // 4-mat
	"matkamie.MOD", // heatbeat
	"jester-chipmunks.MOD", // jester
	"unreeeal_superhero_3-looping_version.XM", // rez+kenet
	"anar11.MOD", // 4-mat
	"external.XM", // Quazar
	"ESTRAYK-Drop.MOD" // Estrayk
};
#define DEFAULT_VOLUME MIX_MAX_VOLUME/2
#define MATKAMIE_VOLUME MIX_MAX_VOLUME
#define LOWER_VOLUME MIX_MAX_VOLUME/3

const int musics_volume[NB_CHIPTUNES] = {
	DEFAULT_VOLUME,
	DEFAULT_VOLUME,
	MATKAMIE_VOLUME,
	LOWER_VOLUME,
	DEFAULT_VOLUME,
	LOWER_VOLUME,
	DEFAULT_VOLUME,
	LOWER_VOLUME
};
#endif

static int ignoreForAbit[NB_WAV];
static int ignoreForAbitFlag[NB_WAV];
int traceMask=DEFAULT_TRACE_MAX;
static bool pic_timeExit=false; //hack in case input is too early in the intro pic

#ifdef __LIBRETRO__
#include <libretro.h>
int16_t *frame_sample_buf;
uint32_t num_samples_per_frame;
retro_audio_sample_batch_t audio_batch_cb;
#endif

bool audio=true;
bool cheatMode=false;
static bool fxTraces=false;
BotTree* tree[nb_dyna];

#ifdef DEBUG
int walkingToCell[nb_dyna];
#endif

#ifdef LOAD_FROM_FILES
int rom_unzip(const char *path, const char *extraction_directory)
{
	path_mkdir(extraction_directory);

	unzFile *zipfile = (unzFile *) unzOpen(path);
	if ( zipfile == NULL )
	{
		log_error("%s: not found\n", path);
		return -1;
	}
	unz_global_info global_info;
	if (unzGetGlobalInfo(zipfile, &global_info) != UNZ_OK)
	{
		log_error("could not read file global info\n");
		unzClose(zipfile);
		return -1;
	}


	char read_buffer[8192];

	uLong i;
	for (i = 0; i < global_info.number_entry; ++i)
	{
		unz_file_info file_info;
		char filename[PATH_MAX_LENGTH];
		if (unzGetCurrentFileInfo(zipfile, &file_info, filename, PATH_MAX_LENGTH,
		                          NULL, 0, NULL, 0 ) != UNZ_OK)
		{
			log_error( "could not read file info\n" );
			unzClose( zipfile );
			return -1;
		}

		const size_t filename_length = strlen(filename);
		if (filename[filename_length-1] == '/')
		{
			log_debug("dir:%s\n", filename);
			char abs_path[PATH_MAX_LENGTH];
			fill_pathname_join(abs_path,
			                   extraction_directory, filename, sizeof(abs_path));
			path_mkdir(abs_path);
		}
		else
		{
			log_debug("file:%s\n", filename);
			if (unzOpenCurrentFile(zipfile) != UNZ_OK)
			{
				log_error("could not open file\n");
				unzClose(zipfile);
				return -1;
			}

			char abs_path[PATH_MAX_LENGTH];
			fill_pathname_join(abs_path,
			                   extraction_directory, filename, sizeof(abs_path));
			FILE *out = fopen(abs_path, "wb");
			if (out == NULL)
			{
				log_error("could not open destination file\n");
				unzCloseCurrentFile(zipfile);
				unzClose(zipfile);
				return -1;
			}

			int error = UNZ_OK;
			do
			{
				error = unzReadCurrentFile(zipfile, read_buffer, 8192);
				if (error < 0)
				{
					log_error("error %d\n", error);
					unzCloseCurrentFile(zipfile);
					unzClose(zipfile);
					return -1;
				}

				if (error > 0)
					fwrite(read_buffer, error, 1, out);

			} while (error > 0);

			fclose(out);
		}

		unzCloseCurrentFile(zipfile);

		if (i + 1  < global_info.number_entry)
		{
			if (unzGoToNextFile(zipfile) != UNZ_OK)
			{
				log_error("cound not read next file\n");
				unzClose(zipfile);
				return -1;
			}
		}
	}
	unzClose(zipfile);
	unlink(path);
	return 0;
}
#endif

bool mrboom_debug_state_failed() {
	static db * saveState=NULL;
	unsigned int i=0;
	bool failed=false;
	if (saveState==NULL) {
		saveState=(uint8_t *) calloc(SIZE_RO_SEGMENT,1);
		memcpy(saveState, &m.FIRST_RO_VARIABLE,  SIZE_RO_SEGMENT);
	} else {
		db * currentMem=&m.FIRST_RO_VARIABLE;
		for (i=0; i<SIZE_RO_SEGMENT; i++) {
			if (saveState[i]!=currentMem[i]) {
				log_error("RO variable changed at %x\n",i+(unsigned int) offsetof(struct Mem,FIRST_RO_VARIABLE));
				memcpy(saveState, &m.FIRST_RO_VARIABLE,  SIZE_RO_SEGMENT);
				failed=true;
			}
		}
	}
	return failed;
}

#ifndef LOAD_FROM_FILES
static unsigned short crc16(const unsigned char* data_p, int length){
	unsigned char x;
	unsigned short crc = 0xFFFF;

	while (length--) {
		x = crc >> 8 ^ *data_p++;
		x ^= x>>4;
		crc = (crc << 8) ^ ((unsigned short)(x << 12)) ^ ((unsigned short)(x <<5)) ^ ((unsigned short)x);
	}
	return crc;
}
#endif

#ifdef __LIBSDL2__
int sdl2_fx_volume=DEFAULT_SDL2_FX_VOLUME;
#endif

bool mrboom_init() {
#ifdef LOAD_FROM_FILES
	char romPath[PATH_MAX_LENGTH];
	char dataPath[PATH_MAX_LENGTH];
	char extractPath[PATH_MAX_LENGTH];
#endif
	asm2C_init();
	if (!m.isLittle) {
		m.isbigendian=1;
	}
	strcpy((char *) &m.iff_file_name,"mrboom.dat");
	m.taille_exe_gonfle=0;
#ifdef __LIBSDL2__
	/* Initialize SDL. */
	if (SDL_Init(SDL_INIT_AUDIO) < 0) {
		log_error("Error SDL_Init\n");
	}

	/* Initialize SDL_mixer */
	if( Mix_OpenAudio( 22050, MIX_DEFAULT_FORMAT, 2, 512 ) == -1 ) {
		log_error("Error Mix_OpenAudio\n");
		audio=false;
	}
	const char * scrolltext="  players can join the game using their action keys...   use the b button (pad) or ctrl to drop a bomb   a (pad) or alt to trigger the bomb remote control   x (pad) or shift to jump with a kangaroo   select (pad) or space to add a bomber-bot   start (pad) or return to start!   check the command lines options to enable team modes   graphics by zaac exocet easy and marblemad   musics by 4-mat carter heatbeat quazar jester estrayk rez and kenet  (c) 1997-2018 remdy software.     (wrap time)  "
	;
#ifdef DEBUG
	int sizeBuffer=offsetof(struct Mem,message1)-offsetof(struct Mem,tecte);
	log_info("%d %d\n",strlen(scrolltext),sizeBuffer);
	assert((strlen(scrolltext)+1)<sizeBuffer);
#endif
	strcpy((char *) m.tecte,scrolltext);
	m.tecte[strlen((char *) m.tecte)]=219;
#endif

#ifndef LOAD_FROM_FILES
	m.dataloaded=1;
	log_debug( "Mrboom: Crc16 heap: %d\n",crc16(m.heap,HEAP_SIZE));
#endif

#ifdef LOAD_FROM_FILES
	char tmpDir[PATH_MAX_LENGTH];
	snprintf(tmpDir, sizeof(tmpDir),"%s","/tmp");
	if (getenv("HOME")!=NULL) {
		snprintf(tmpDir, sizeof(tmpDir),"%s",getenv("HOME"));
	}
	if (getenv("TMP")!=NULL) {
		snprintf(tmpDir, sizeof(tmpDir),"%s",getenv("TMP"));
	}
	if (getenv("TEMP")!=NULL) {
		snprintf(tmpDir, sizeof(tmpDir),"%s",getenv("TEMP"));
	}
	snprintf(romPath, sizeof(romPath), "%s/mrboom.rom", tmpDir);
	snprintf(extractPath, sizeof(extractPath), "%s/mrboom", tmpDir);

	log_debug("romPath: %s\n", romPath);
	if (filestream_write_file(romPath, dataRom, sizeof(dataRom))==false) {
		log_error("Error writing %s\n",romPath);
		return false;
	}
	rom_unzip(romPath, extractPath);
	unlink(romPath);
	m.path=strdup(extractPath);
	char tmp[PATH_MAX_LENGTH];
#endif
	for (int i=0; i<NB_WAV; i++) {
#ifdef LOAD_FROM_FILES
		sprintf(tmp,"%s/%d.WAV",extractPath,i);
#ifdef __LIBRETRO__
		wave[i] = audio_mix_load_wav_file(&tmp[0], SAMPLE_RATE);
#endif
#ifdef __LIBSDL2__
		if (audio) {
			wave[i] = Mix_LoadWAV(tmp);
			Mix_VolumeChunk(wave[i], MIX_MAX_VOLUME*sdl2_fx_volume/10);
		}
#endif
		unlink(tmp);
		if (wave[i]==NULL) {
			log_error( "cant load %s\n",tmp);
		}
#endif
		ignoreForAbit[i]=0;
		ignoreForAbitFlag[i]=5;
	}
#ifdef __LIBSDL2__


	for(int i=0; i<NB_CHIPTUNES; i++) {
		sprintf(tmp,"%s/%s",extractPath,musics_filenames[i]);
		if (audio) {
			musics[i] = Mix_LoadMUS(tmp);
			if(!musics[i]) {
				log_error("Mix_LoadMUS(\"%s\"): %s: please check SDL2_mixer is compiled --with-libmodplug\n", musics_filenames[i], Mix_GetError());
				return false;
			}
		}
		unlink(tmp);
	}

#endif

	ignoreForAbitFlag[0]=30;
	ignoreForAbitFlag[10]=30; // Kangaroo jump
	ignoreForAbitFlag[13]=30;
	ignoreForAbitFlag[14]=30;

	for (int i=0; i<keyboardDataSize*nb_dyna; i++) {
		if (!((i+1)%keyboardDataSize)) {
			m.touches_[i]=-1;
		} else {
			m.touches_[i]=i+keyboardCodeOffset;
		}
	}
	program();

#ifdef DUMP_HEAP
	filestream_write_file("/tmp/heap", m.heap, HEAP_SIZE);
#endif

	m.nosetjmp=1; //will go to menu, except if state loaded after

#ifdef LOAD_FROM_FILES
	snprintf(dataPath, sizeof(dataPath), "%s/mrboom.dat", extractPath);
	log_debug("dataPath = %s \n",dataPath);
	unlink(dataPath);
	log_debug("extractPath = %s \n",extractPath);
	rmdir(extractPath);
#endif

#ifdef DEBUG
	asm2C_printOffsets(offsetof(struct Mem,FIRST_RW_VARIABLE));
#endif
	for (int i=0; i<nb_dyna; i++) {
		tree[i]=new BotTree(i);
	}
	return true;
}

void mrboom_deinit() {
#ifdef LOAD_FROM_FILES
	/* free WAV */
	for (int i=0; i<NB_WAV; i++)
	{
#ifdef __LIBRETRO__
		audio_mix_free_chunk(wave[i]);
#endif
#ifdef __LIBSDL2__
		Mix_FreeChunk(wave[i]);
#endif
	}
#endif
}

void mrboom_sound(void)
{
	if (!audio) return;

	static int last_voice=0;
	for (int i=0; i<NB_WAV; i++)
	{
		if (ignoreForAbit[i])
			ignoreForAbit[i]--;
	}

	while (m.last_voice!=(unsigned) last_voice)
	{
		db a=*(((db *) &m.blow_what2[last_voice/2]));
		db a1=a&0xf;
		if (fxTraces) log_debug("blow what: sample = %d / panning %d, note: %d ignoreForAbit[%d]\n",a1,(db) a>>4,(db)(*(((db *) &m.blow_what2[last_voice/2])+1)),ignoreForAbit[a1]);
		last_voice=(last_voice+2)%NB_VOICES;
#ifdef LOAD_FROM_FILES
		if ((a1>=0) && (a1<NB_WAV) && (wave[a1]!=NULL))
#else
		if ((a1>=0) && (a1<NB_WAV))
#endif
		{
			bool dontPlay=0;

			if (ignoreForAbit[a1])
			{
				if (fxTraces) log_debug("Ignore sample id %d\n",a1);
				dontPlay=1;
			}

			if (dontPlay == 0)
			{
#ifdef __LIBRETRO__
#ifdef LOAD_FROM_FILES
				frames_left[a1] = audio_mix_get_chunk_num_samples(wave[a1]);
#else
				frames_left[a1] = wave[a1].num_samples;
#endif
#endif
#ifdef __LIBSDL2__
				if ( Mix_PlayChannel(-1, wave[a1], 0) == -1 )
				{
					if (fxTraces) log_error("Error playing sample id %d.\n",a1);
				}
#endif


#ifdef __LIBRETRO__
				// special message on failing to start a game...
				if (a1==14)
				{
					show_message("Press A to join!");
				}
#endif
				ignoreForAbit[a1]=ignoreForAbitFlag[a1];
			}
		}
		else
		{
			log_error("Wrong sample id %d or NULL.\n",a1);
		}
	}
#ifdef __LIBSDL2__
	if (music) {
		static int currentLevel=-2;
		if (level()!=currentLevel) {
			int index=musics_index;
			currentLevel=level();
			if (currentLevel==-1) index=0;
			Mix_VolumeMusic(musics_volume[index]);
			log_debug("Playing %s volume:%d\n", musics_filenames[index],Mix_VolumeMusic(-1));
			if ( Mix_PlayMusic( musics[index], -1) == -1 ) {
				log_error("error playing music %d\n",musics[0]);
			}
			if (index) {
				musics_index=(musics_index+1)%(NB_CHIPTUNES);
			}
			if (!musics_index) musics_index++;
		}
	}
#endif
}
void mrboom_reset_special_keys() {
	db * keys=m.total_t;
	for (int i=0; i<nb_dyna; i++) {
		int pointeurSelect=64+5+i*7;
		*(keys+pointeurSelect)=0;
	}
	*(keys+8*7)=0;
	*(keys+8*7+1)=0;
	*(keys+8*7+2)=0; // une_touche_a_telle_ete_pressee
	if ((pic_timeExit) && (m.pic_time)) *(keys+8*7+2)=1;
}

void mrboom_update_input(int keyid, int playerNumber,int state, bool isIA)
{
	static int selectPressed=0;
	static int selectPressedPlayerNumber=0;
	static int startPressed=0;

	db * keys=m.total_t;
	if (isIA) keys+=64;

	switch (keyid)
	{
	case button_down:
		*(keys+3+playerNumber*7)=state;
		break;
	case button_right:
		*(keys+1+playerNumber*7)=state;
		break;
	case button_left:
		*(keys+2+playerNumber*7)=state;
		break;
	case button_up:
		*(keys+playerNumber*7)=state;
		break;
	case button_a:
		*(keys+5+playerNumber*7)=state;
		break;
	case button_select:
		if (selectPressed!=1) {
			if (state) {
				addOneAIPlayer();
			}
			selectPressedPlayerNumber=playerNumber;
		}
		if (selectPressedPlayerNumber==playerNumber) {
			selectPressed=state;
		}
		break;
	case button_start:
		if (state) {
			*(keys+8*7)=state;
		}
		startPressed=state;
		break;
	case button_b:
		*(keys+4+playerNumber*7)=state;
		break;
	case button_x:
		*(keys+6+playerNumber*7)=state;
		break;
	case button_l:
		if (cheatMode) {
			if (state) {
				activeCheatMode();
			}
		}
		break;
	case button_r:
		if (cheatMode) {
			if (state) {
				activeApocalypse();
			}
		}
		break;
	}

	if (state) {
		*(keys+8*7+2)=1; // une_touche_a_telle_ete_pressee
		if (m.pic_time) pic_timeExit=true;
	}
	if (startPressed && selectPressed) {
		pressESC();
	}
}

#ifdef __LIBRETRO__
void audio_callback(void)
{
	unsigned i;
	if (!audio_batch_cb)
		return;

	memset(frame_sample_buf, 0, num_samples_per_frame * 2 * sizeof(int16_t));

	for (i = 0; i < NB_WAV; i++)
	{
		if (frames_left[i])
		{
			unsigned j;
			unsigned frames_to_copy = 0;
#ifdef LOAD_FROM_FILES
			int16_t *samples = audio_mix_get_chunk_samples(wave[i]);
			unsigned num_frames = audio_mix_get_chunk_num_samples(wave[i]);
#else
			const int16_t *samples = wave[i].samples;
			unsigned num_frames = wave[i].num_samples;
#endif
			frames_to_copy = MIN(frames_left[i], num_samples_per_frame);

			for (j = 0; j < frames_to_copy; j++)
			{
				unsigned chunk_size = num_frames * 2;
				unsigned sample = frames_left[i] * 2;
				frame_sample_buf[j * 2] = CLAMP_I16(frame_sample_buf[j * 2] + (samples[chunk_size - sample] >> SOUND_VOLUME));
				frame_sample_buf[(j * 2) + 1] = CLAMP_I16(frame_sample_buf[(j * 2) + 1] + (samples[(chunk_size - sample) + 1] >> SOUND_VOLUME));
				frames_left[i]--;
			}
		}
	}

	audio_batch_cb(frame_sample_buf, num_samples_per_frame);
}
#endif

void mrboom_deal_with_autofire() {
	if (autofire()==false) {
		if (isGameActive()) {
			for (int i=0; i<numberOfPlayers(); i++) {
				if (isAIActiveForPlayer(i)==false) {
					if (bombInCell(xPlayer(i),yPlayer(i))) {
						mrboom_update_input(button_b,i,0,false);
					}
				}
			}
		}
	}
}

#ifdef DEBUG
BotState botStates[nb_dyna];
#endif

void mrboom_tick_ai() {
	for (int i=0; i<numberOfPlayers(); i++) {
		if (isGameActive()) {
#ifdef DEBUG
			walkingToCell[i]=0;
			botStates[i]=goingNowhere;
#endif
			if (isAIActiveForPlayer(i) && isAlive(i)) {
				tree[i]->updateGrids();
				tree[i]->tick();
			}
		} else {
			if (isAIActiveForPlayer(i)) {
				mrboom_update_input(button_a,i,frameNumber()%4,true); // to press A inside the menu...
				tree[i]->initBot();
			}
		}
	}
#if 0
	if (isGameActive()) {
		static int similarityScore=0;
		static int similarityScoreBonus=0;
		static int similarityScoreBomb=0;
		static int similarityScoreSafe=0;
		static int similarityScore2=0;
		static int similarityScoreBonus2=0;
		static int similarityScoreBomb2=0;
		static int similarityScoreSafe2=0;

		for (int i=0; i<numberOfPlayers(); i++) {
			int target=walkingToCell[i];
			int nb=0;
			int nb2=0;
			for (int z=0; z<numberOfPlayers(); z++) {
				if ((target) && (walkingToCell[z]==target)) {
					nb++;
					if (cellPlayer(z)==cellPlayer(i)) {
						nb2++;
					}
				}
			}
			if (nb>1) {
				similarityScore+=(nb-1);
				if (botStates[i]==goingSafe) {
					similarityScoreSafe+=(nb-1);
				}
				if (botStates[i]==goingBonus) {
					similarityScoreBonus+=(nb-1);

				}
				if (botStates[i]==goingBomb) {
					similarityScoreBomb+=(nb-1);
				}
				log_info("%d %d AI walking to %d (similarityScore:%d %d Safe:%d Bonus:%d Bomb:%d)\n",frameNumber(),nb,target,similarityScore,(similarityScore2*10000)/(1+frameNumber()),similarityScoreSafe,similarityScoreBonus,similarityScoreBomb);
				if (nb2>1) {
					similarityScore2+=(nb2-1);
					if (botStates[i]==goingSafe) {
						similarityScoreSafe2+=(nb2-1);
					}
					if (botStates[i]==goingBonus) {
						similarityScoreBonus2+=(nb2-1);

					}
					if (botStates[i]==goingBomb) {
						similarityScoreBomb2+=(nb2-1);
					}
				}
				log_info("%d %d AI walking to %d (similarityScore2:%d %d Safe2:%d Bonus2:%d Bomb2:%d)\n",frameNumber(),nb2,target,similarityScore2,(similarityScore2*10000)/(1+frameNumber()),similarityScoreSafe2,similarityScoreBonus2,similarityScoreBomb2);
			}
		}
	}
#endif

	#if 0
	printCellInfo(213);
	if (isGameActive()) {
		for (int i=0; i<numberOfPlayers(); i++) {
			if (isAIActiveForPlayer(i)) {
				tree[i]->printCellInfo(213);
			}
		}
	}
	#endif
}

bool debugTracesPlayer(int player) {
	return ((1 << player) & traceMask);
}



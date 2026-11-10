#include <stdio.h>
#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "diskio_impl.h"
#include "diskio_sdmmc.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include "esp_log.h"

#ifdef CONFIG_SPIRAM
#error "SD CMD GPIO37 conflicts with Octal PSRAM: disable CONFIG_SPIRAM"
#endif

static const char *TAG = "sd-player";
#define LINE_COUNT 2
static i2s_chan_handle_t outputs[LINE_COUNT];
static QueueHandle_t commands[LINE_COUNT];
static SemaphoreHandle_t lock, storage_lock, settings_lock;
static nvs_handle_t settings_handle;
static bool mounted;
static bool sd_ready, formatting, uploading, command_active[LINE_COUNT];
static sdmmc_card_t sd_card;
static FATFS *sd_fs;
static BYTE sd_drive;
static char sd_path[3];
static char format_result[256];
static char storage_error[256];
typedef struct {
  bool playing, paused, autoplay;
  int volume;
  char track[256], error[256];
  uint32_t position, length, rate, loops;
  uint16_t channels, bits;
} player_state;
static player_state players[LINE_COUNT] = {{.volume=25},{.volume=25}};
typedef struct { char action[16], file[256]; int volume; bool start_paused; } command;
typedef struct {
  uint32_t version;
  char track[256];
  uint8_t volume, enabled, paused;
} saved_line;
static saved_line saved[LINE_COUNT];
static const char *settings_keys[LINE_COUNT]={"line1","line2"};
typedef struct {
  uint32_t rate, bytes;
  uint16_t channels, bits, format, block_align;
} wav_info;

static uint16_t u16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static uint32_t u32(const unsigned char *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1]<<8) |
         ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static bool valid_name(const char *s) {
  size_t n = strlen(s);
  for (size_t i=0;i<n;i++)
    if ((unsigned char)s[i]<32 || (unsigned char)s[i]==127) return false;
  return n > 4 && n < 256 && !strchr(s, '/') && !strchr(s, '\\') &&
         !strstr(s, "..") && !strcasecmp(s+n-4, ".wav");
}

static void set_line_error(int id,const char *message) {
  xSemaphoreTake(lock,portMAX_DELAY);
  snprintf(players[id].error,sizeof(players[id].error),"%s",message);
  xSemaphoreGive(lock);
}

// Called with lock held. A paused line still owns an open SD file.
static bool all_players_idle(void) {
  for (int i=0;i<LINE_COUNT;i++)
    if (players[i].playing || command_active[i] || uxQueueMessagesWaiting(commands[i])) return false;
  return true;
}

static void load_settings(void) {
  ESP_ERROR_CHECK(nvs_open("sd_player",NVS_READWRITE,&settings_handle));
  for (int i=0;i<LINE_COUNT;i++) {
    size_t size=sizeof(saved_line);
    saved_line value={0};
    esp_err_t err=nvs_get_blob(settings_handle,settings_keys[i],&value,&size);
    if (err!=ESP_OK || size!=sizeof(value) || value.version!=1 ||
        !memchr(value.track,0,sizeof(value.track)) || value.volume>100 ||
        value.enabled>1 || value.paused>1 ||
        (value.track[0] && !valid_name(value.track))) {
      value=(saved_line){.version=1,.volume=25};
    }
    saved[i]=value;
    players[i].volume=value.volume;
    players[i].autoplay=value.enabled && value.track[0];
    players[i].paused=value.paused && players[i].autoplay;
    snprintf(players[i].track,sizeof(players[i].track),"%s",value.track);
  }
}

static void save_line(int id) {
  saved_line value={.version=1};
  xSemaphoreTake(lock,portMAX_DELAY);
  value.volume=players[id].volume;
  value.enabled=players[id].autoplay;
  value.paused=players[id].paused;
  snprintf(value.track,sizeof(value.track),"%s",players[id].track);
  xSemaphoreGive(lock);
  xSemaphoreTake(settings_lock,portMAX_DELAY);
  // Never write NVS on loop wrap or periodic status polling.
  esp_err_t err=ESP_OK;
  if (saved[id].volume!=value.volume || saved[id].enabled!=value.enabled ||
      saved[id].paused!=value.paused || strcmp(saved[id].track,value.track)) {
    err=nvs_set_blob(settings_handle,settings_keys[id],&value,sizeof(value));
    if (err==ESP_OK) err=nvs_commit(settings_handle);
    if (err==ESP_OK) saved[id]=value;
  }
  xSemaphoreGive(settings_lock);
  if (err!=ESP_OK) {
    ESP_LOGE(TAG,"Saving line %d: %s",id+1,esp_err_to_name(err));
    set_line_error(id,"Не удалось сохранить выбор в памяти. Воспроизведение доступно, но настройки могут забыться после перезагрузки.");
  }
}
// Parse RIFF chunks, including metadata and padding; reject truncated files.
static const char *read_wav(FILE *f, wav_info *w) {
  unsigned char h[40];
  memset(w,0,sizeof(*w));
  if (fseek(f, 0, SEEK_END)) return "Ошибка чтения размера файла с SD.";
  long size = ftell(f);
  rewind(f);
  if (size < 12 || fread(h,1,12,f)!=12 || memcmp(h,"RIFF",4) ||
      memcmp(h+8,"WAVE",4)) return "Это не RIFF WAV. Простое переименование MP3 в .wav не меняет формат.";
  uint64_t end = (uint64_t)u32(h+4)+8;
  if (end > (uint64_t)size || end < 12) return "WAV обрезан: размер в заголовке больше записанного файла.";
  bool fmt = false;
  long data = -1;
  while ((uint64_t)ftell(f)+8 <= end) {
    if (fread(h,1,8,f)!=8) return "Не удалось прочитать заголовок WAV с карты.";
    uint32_t n=u32(h+4);
    long start=ftell(f);
    uint64_t next=(uint64_t)start+n+(n&1);
    if (next>end) return "Повреждён WAV: аудиоблок выходит за границы файла.";
    if (!memcmp(h,"fmt ",4)) {
      if (n<16 || fread(h,1,16,f)!=16) return "Повреждён блок fmt WAV.";
      w->format=u16(h); w->channels=u16(h+2); w->rate=u32(h+4);
      w->block_align=u16(h+12); w->bits=u16(h+14);
      uint32_t byte_rate=u32(h+8);
      if (w->format==0xfffe) {
        const unsigned char guid_tail[]={0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71};
        if (n<40 || fread(h+16,1,24,f)!=24 || u16(h+16)<22 ||
            u16(h+16)>n-18 || memcmp(h+28,guid_tail,sizeof(guid_tail)))
          return "Неподдерживаемый заголовок WAV Extensible.";
        uint32_t subtype=u32(h+24);
        if (subtype!=1 && subtype!=3) return "WAV Extensible содержит сжатое аудио. Нужен PCM или float32.";
        w->format=subtype;
        if (!u16(h+18) || u16(h+18)>w->bits) return "Некорректная разрядность WAV Extensible.";
      }
      ESP_LOGI(TAG,"WAV format=%u, %u Hz, %u channels, %u bits",w->format,(unsigned)w->rate,w->channels,w->bits);
      if (w->channels!=1 && w->channels!=2) return "Поддерживаются только WAV моно и стерео.";
      if (w->rate<8000 || w->rate>48000) return "Частота WAV вне диапазона 8000–48000 Гц. Преобразуйте в 44100 или 48000 Гц.";
      if (w->format!=1 || w->bits!=16)
        return "Нужен WAV PCM 16 бит. Преобразуйте файл через audio-converter.html и загрузите результат.";
      if (w->block_align!=w->channels*(w->bits/8) ||
          byte_rate!=w->rate*w->block_align) return "Некорректный размер аудиокадра в заголовке WAV.";
      fmt=true;
    } else if (!memcmp(h,"data",4)) { data=start; w->bytes=n; }
    if (fmt && data>=0) break;
    if (fseek(f,(long)next,SEEK_SET)) return "Ошибка перехода между блоками WAV.";
  }
  if (!fmt || data<0) return "В WAV нет блока формата fmt или аудиоданных data.";
  if (!w->bytes || w->bytes%w->block_align) return "Аудиоданные WAV пусты или содержат неполный кадр.";
  if (fseek(f,data,SEEK_SET)) return "Не удалось начать чтение аудиоданных WAV.";
  return NULL;
}

static void set_error(const char *s) {
  xSemaphoreTake(lock,portMAX_DELAY);
  snprintf(storage_error,sizeof(storage_error),"%s",s);
  xSemaphoreGive(lock);
}

static void init_sd(void) {
  sdmmc_host_t host=SDMMC_HOST_DEFAULT();
  host.flags &= ~(SDMMC_HOST_FLAG_8BIT | SDMMC_HOST_FLAG_DDR);
  sdmmc_slot_config_t slot=SDMMC_SLOT_CONFIG_DEFAULT();
  slot.width=4; slot.clk=38; slot.cmd=37;
  slot.d0=39; slot.d1=40; slot.d2=35; slot.d3=36;
  esp_err_t err=sdmmc_host_init();
  if (err!=ESP_OK) goto fail;
  err=sdmmc_host_init_slot(host.slot,&slot);
  if (err==ESP_OK) err=sdmmc_card_init(&host,&sd_card);
  if (err!=ESP_OK) { sdmmc_host_deinit(); goto fail; }
  err=ff_diskio_get_drive(&sd_drive);
  if (err!=ESP_OK) { sdmmc_host_deinit(); goto fail; }
  ff_diskio_register_sdmmc(sd_drive,&sd_card);
  ff_sdmmc_set_disk_status_check(sd_drive,true);
  assert(sd_drive<10);
  sd_path[0]='0'+sd_drive; sd_path[1]=':'; sd_path[2]=0;
  const esp_vfs_fat_conf_t conf={
    .base_path="/sd",.fat_drive=sd_path,.max_files=4
  };
  err=esp_vfs_fat_register_cfg(&conf,&sd_fs);
  if (err!=ESP_OK) {
    ff_diskio_unregister(sd_drive); sdmmc_host_deinit(); goto fail;
  }
  // Keep the raw device registered even when its filesystem is broken/absent.
  // This lets the user explicitly format such a card from the web interface.
  sd_ready=true;
  FRESULT result=f_mount(sd_fs,sd_path,1);
  mounted=result==FR_OK;
  sdmmc_card_print_info(stdout,&sd_card);
  if (!mounted) {
    ESP_LOGW(TAG,"SD detected, FAT mount failed: %d",result);
    set_error("Карта обнаружена, но файловая система недоступна. Можно отформатировать её в FAT32.");
  }
  return;
fail:
  ESP_LOGE(TAG,"SD initialization: %s",esp_err_to_name(err));
  set_error("Карта не отвечает. Проверьте карту и контакты, затем перезапустите плату.");
}

// Called only by line 1's task, with both lines stopped and queues empty.
// storage_lock excludes directory scans; no FAT handles survive this point.
static void format_sd(void) {
  xSemaphoreTake(storage_lock,portMAX_DELAY);
  void *work=malloc(4096);
  FRESULT result=FR_NOT_ENOUGH_CORE;
  bool now_mounted;
  xSemaphoreTake(lock,portMAX_DELAY); now_mounted=mounted; xSemaphoreGive(lock);
  if (work) {
    result=f_mount(NULL,sd_path,0);
    now_mounted=false;
    if (result==FR_OK) {
      LBA_t partitions[]={100,0,0,0};
      result=f_fdisk(sd_drive,partitions,work);
    }
    if (result==FR_OK) {
      // Exactly FAT32, two FAT copies; FatFs chooses an appropriate cluster size.
      const MKFS_PARM options={.fmt=FM_FAT32,.n_fat=2,.au_size=0};
      result=f_mkfs(sd_path,&options,work,4096);
    }
    FRESULT mount_result=f_mount(sd_fs,sd_path,1);
    now_mounted=mount_result==FR_OK;
    if (result==FR_OK) result=mount_result;
    if (result==FR_OK && sd_fs->fs_type!=FS_FAT32) result=FR_INT_ERR;
    free(work);
  }
  ESP_LOGI(TAG,"FAT32 format result: %d, mounted: %d",result,now_mounted);
  xSemaphoreTake(lock,portMAX_DELAY);
  mounted=now_mounted;
  if (result==FR_OK) {
    snprintf(format_result,sizeof(format_result),"Готово: карта отформатирована в FAT32. Можно записать WAV-файлы.");
    storage_error[0]=0;
    for (int i=0;i<LINE_COUNT;i++) {
      players[i].track[0]=0; players[i].autoplay=false;
      players[i].paused=false; players[i].error[0]=0;
    }
  } else {
    snprintf(storage_error,sizeof(storage_error),"Форматирование не удалось (FatFs %d). Проверьте карту и журнал UART.",result);
    format_result[0]=0;
  }
  xSemaphoreGive(lock);
  if (result==FR_OK) for (int i=0;i<LINE_COUNT;i++) save_line(i);
  xSemaphoreTake(lock,portMAX_DELAY); formatting=false; xSemaphoreGive(lock);
  xSemaphoreGive(storage_lock);
}

static void player_task(void *arg) {
  int id=(int)(intptr_t)arg;
  i2s_chan_handle_t tx=outputs[id];
  FILE *f=NULL;
  wav_info wav={0};
  long data_offset=0;
  uint32_t remaining=0;
  bool enabled=false;
  unsigned char input[2048];
  int16_t output[1024];
  for (;;) {
    command cmd;
    // Dequeue and mark active atomically with storage-operation reservations.
    xSemaphoreTake(lock,portMAX_DELAY);
    bool have_command=xQueueReceive(commands[id],&cmd,0)==pdTRUE;
    command_active[id]=have_command;
    xSemaphoreGive(lock);
    if (have_command) {
      if (!strcmp(cmd.action,"volume")) {
        xSemaphoreTake(lock,portMAX_DELAY); players[id].volume=cmd.volume;
        xSemaphoreGive(lock);
      } else if (!strcmp(cmd.action,"pause")) {
        xSemaphoreTake(lock,portMAX_DELAY);
        if (players[id].playing) players[id].paused=!players[id].paused;
        xSemaphoreGive(lock);
      } else {
        if (enabled) { i2s_channel_disable(tx); enabled=false; }
        if (f) { fclose(f); f=NULL; }
        xSemaphoreTake(lock,portMAX_DELAY);
        players[id].playing=false; players[id].paused=false; players[id].position=0;
        players[id].length=0; players[id].loops=0; players[id].error[0]=0;
        if (!strcmp(cmd.action,"stop")) players[id].autoplay=false;
        xSemaphoreGive(lock);
        if (!strcmp(cmd.action,"format")) {
          format_sd();
        } else if (!strcmp(cmd.action,"play")) {
          char path[264]; snprintf(path,sizeof(path),"/sd/%s",cmd.file);
          f=fopen(path,"rb");
          char open_error[256];
          const char *problem;
          if (!f) {
            snprintf(open_error,sizeof(open_error),"Не удалось открыть файл на SD: %s (errno %d).",strerror(errno),errno);
            problem=open_error;
          } else problem=read_wav(f,&wav);
          if (problem) {
            ESP_LOGE(TAG,"Play %s: %s",cmd.file,problem);
            if (f) fclose(f);
            f=NULL; set_line_error(id,problem);
          } else {
            i2s_std_clk_config_t clk=I2S_STD_CLK_DEFAULT_CONFIG(wav.rate);
            esp_err_t err=i2s_channel_reconfig_std_clock(tx,&clk);
            if (err==ESP_OK) err=i2s_channel_enable(tx);
            if (err!=ESP_OK) {
              fclose(f); f=NULL; set_line_error(id,"Ошибка настройки I2S.");
            } else {
              enabled=true; remaining=wav.bytes; data_offset=ftell(f);
              xSemaphoreTake(lock,portMAX_DELAY);
              players[id].playing=true; players[id].autoplay=true;
              players[id].paused=cmd.start_paused; players[id].rate=wav.rate;
              players[id].channels=wav.channels; players[id].bits=wav.bits; players[id].length=wav.bytes;
              snprintf(players[id].track,sizeof(players[id].track),"%s",cmd.file);
              xSemaphoreGive(lock);
            }
          }
        }
      }
      if (strcmp(cmd.action,"format")) save_line(id);
    }
    xSemaphoreTake(lock,portMAX_DELAY);
    command_active[id]=false;
    bool paused=players[id].paused;
    int volume=players[id].volume;
    xSemaphoreGive(lock);
    if (!f) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
    if (paused) {
      // Clock silence so a paused stream never repeats stale DMA samples.
      memset(output,0,1024);
      size_t sent=0;
      if (enabled) i2s_channel_write(tx,output,1024,&sent,100);
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    // Repeat only the data chunk. Keep I2S enabled and its DMA queue intact.
    // Neither the RIFF header nor metadata/zero-padding reaches the speakers.
    if (!remaining) {
      if (fseek(f,data_offset,SEEK_SET)) {
        set_line_error(id,"Не удалось вернуться к началу звука на SD.");
        goto playback_failed;
      }
      remaining=wav.bytes;
      xSemaphoreTake(lock,portMAX_DELAY);
      players[id].position=0; players[id].loops++;
      xSemaphoreGive(lock);
    }
    // Bound by complete source frames; output is always stereo PCM16.
    size_t wanted=512*wav.block_align;
    if (wanted>remaining) wanted=remaining;
    size_t n=fread(input,1,wanted,f);
    if (n!=wanted) {
      set_line_error(id,"Ошибка чтения карты или WAV обрезан. Эта линия остановлена.");
      goto playback_failed;
    }
    if (n%wav.block_align) {
      set_line_error(id,"Неполный аудиокадр в WAV.");
      goto playback_failed;
    }
    size_t frames=n/wav.block_align, count=0;
    for (size_t i=0;i<frames;i++) {
      const unsigned char *frame=input+i*wav.block_align;
      int16_t left=(int16_t)u16(frame);
      int16_t right=wav.channels==1?left:(int16_t)u16(frame+2);
      output[count++]=(int16_t)((int32_t)left*volume/100);
      output[count++]=(int16_t)((int32_t)right*volume/100);
    }
    size_t offset=0, bytes=count*2;
    while (offset<bytes) {
      size_t sent=0;
      esp_err_t err=i2s_channel_write(tx,(char*)output+offset,bytes-offset,&sent,200);
      offset+=sent;
      if (err!=ESP_OK || !sent) {
        set_line_error(id,"Ошибка вывода I2S. Эта линия остановлена.");
        goto playback_failed;
      }
    }
    remaining=remaining>=n?remaining-n:0;
    xSemaphoreTake(lock,portMAX_DELAY);
    players[id].position=wav.bytes-remaining;
    xSemaphoreGive(lock);
    continue;
playback_failed:
    if (f) { fclose(f); f=NULL; }
    if (enabled) { i2s_channel_disable(tx); enabled=false; }
    xSemaphoreTake(lock,portMAX_DELAY); players[id].playing=false;
    xSemaphoreGive(lock);
  }
}

static esp_err_t json_reply(httpd_req_t *req,cJSON *j) {
  char *body=cJSON_PrintUnformatted(j); cJSON_Delete(j);
  if (!body) return httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"No memory");
  httpd_resp_set_type(req,"application/json; charset=utf-8");
  httpd_resp_set_hdr(req,"Cache-Control","no-store");
  esp_err_t err=httpd_resp_sendstr(req,body); free(body); return err;
}
static esp_err_t fail(httpd_req_t *req,const char *status,const char *msg) {
  httpd_resp_set_status(req,status);
  cJSON *j=cJSON_CreateObject(); cJSON_AddStringToObject(j,"error",msg);
  return json_reply(req,j);
}
static esp_err_t status_handler(httpd_req_t *req) {
  xSemaphoreTake(lock,portMAX_DELAY);
  player_state snapshot[LINE_COUNT]; memcpy(snapshot,players,sizeof(snapshot));
  bool idle=all_players_idle();
  bool is_mounted=mounted, is_formatting=formatting, is_uploading=uploading;
  char result[sizeof(format_result)]; memcpy(result,format_result,sizeof(result));
  char error[sizeof(storage_error)]; memcpy(error,storage_error,sizeof(error));
  xSemaphoreGive(lock);
  cJSON *j=cJSON_CreateObject();
  cJSON_AddBoolToObject(j,"mounted",is_mounted);
  cJSON_AddBoolToObject(j,"detected",sd_ready);
  cJSON_AddBoolToObject(j,"formatting",is_formatting);
  cJSON_AddBoolToObject(j,"uploading",is_uploading);
  cJSON_AddStringToObject(j,"format_result",result);
  cJSON_AddStringToObject(j,"error",error);
  cJSON_AddBoolToObject(j,"idle",idle);
  cJSON_AddNumberToObject(j,"capacity_mb",sd_ready?
      (double)sd_card.csd.capacity*sd_card.csd.sector_size/(1024*1024):0);
  cJSON_AddNumberToObject(j,"bus_width",sd_ready?(1<<sd_card.log_bus_width):0);
  cJSON *lines=cJSON_AddArrayToObject(j,"lines");
  for (int i=0;i<LINE_COUNT;i++) {
    player_state *s=&snapshot[i];
    cJSON *line=cJSON_CreateObject();
    cJSON_AddNumberToObject(line,"id",i+1);
    cJSON_AddBoolToObject(line,"playing",s->playing);
    cJSON_AddBoolToObject(line,"paused",s->paused);
    cJSON_AddBoolToObject(line,"autoplay",s->autoplay);
    cJSON_AddBoolToObject(line,"loop",true);
    cJSON_AddNumberToObject(line,"loops",s->loops);
    cJSON_AddNumberToObject(line,"volume",s->volume);
    cJSON_AddStringToObject(line,"track",s->track);
    cJSON_AddStringToObject(line,"error",s->error);
    double scale=s->rate?s->rate*s->channels*(s->bits/8.0):1;
    cJSON_AddNumberToObject(line,"position",s->position/scale);
    cJSON_AddNumberToObject(line,"duration",s->length/scale);
    cJSON_AddItemToArray(lines,line);
  }
  return json_reply(req,j);
}
static esp_err_t files_handler(httpd_req_t *req) {
  // Never wait behind a long format on the HTTP task: status must stay usable.
  if (!xSemaphoreTake(storage_lock,0))
    return fail(req,"409 Conflict","Карта занята форматированием.");
  xSemaphoreTake(lock,portMAX_DELAY);
  bool available=mounted && !formatting && !uploading;
  xSemaphoreGive(lock);
  if (!available) {
    xSemaphoreGive(storage_lock);
    return fail(req,"503 Service Unavailable","Файловая система карты недоступна.");
  }
  FF_DIR directory;
  FRESULT result=f_opendir(&directory,sd_path);
  if (result!=FR_OK) {
    ESP_LOGE(TAG,"SD directory open failed: FatFs %d",result);
    xSemaphoreGive(storage_lock);
    return fail(req,"500 Internal Server Error","Не удалось прочитать карту.");
  }
  cJSON *j=cJSON_CreateObject(), *files=cJSON_AddArrayToObject(j,"files");
  cJSON *other=cJSON_AddArrayToObject(j,"other_files");
  FILINFO entry;
  int count=0, skipped=0;
  while (count<128) {
    result=f_readdir(&directory,&entry);
    if (result!=FR_OK || !entry.fname[0]) break;
    if (entry.fattrib&AM_DIR) continue;
    if (valid_name(entry.fname)) {
      cJSON_AddItemToArray(files,cJSON_CreateString(entry.fname)); count++;
    } else if (skipped++<16) {
      cJSON_AddItemToArray(other,cJSON_CreateString(entry.fname));
    }
  }
  f_closedir(&directory);
  xSemaphoreGive(storage_lock);
  if (result!=FR_OK) {
    ESP_LOGE(TAG,"SD directory read failed: FatFs %d",result);
    cJSON_Delete(j);
    return fail(req,"500 Internal Server Error","Ошибка чтения каталога SD. Проверьте карту и журнал UART.");
  }
  cJSON_AddStringToObject(j,"directory","/");
  return json_reply(req,j);
}
static esp_err_t control_handler(httpd_req_t *req) {
  char body[768];
  if (req->content_len<=0 || req->content_len>=sizeof(body))
    return fail(req,"400 Bad Request","Некорректный размер команды.");
  int total=0;
  while (total<req->content_len) {
    int n=httpd_req_recv(req,body+total,req->content_len-total);
    if (n<=0) return fail(req,"408 Request Timeout","Команда не получена.");
    total+=n;
  }
  body[total]=0; cJSON *j=cJSON_Parse(body);
  const cJSON *a=cJSON_GetObjectItemCaseSensitive(j,"action");
  const cJSON *line=cJSON_GetObjectItemCaseSensitive(j,"line");
  int id=0;
  if (line) {
    if (!cJSON_IsNumber(line) || line->valuedouble!=line->valueint ||
        line->valueint<1 || line->valueint>LINE_COUNT) {
      cJSON_Delete(j); return fail(req,"400 Bad Request","Номер линии должен быть 1 или 2.");
    }
    id=line->valueint-1;
  }
  command cmd={0};
  if (!cJSON_IsString(a)) { cJSON_Delete(j); return fail(req,"400 Bad Request","Нет действия."); }
  if (!strcmp(a->valuestring,"play")) {
    const cJSON *f=cJSON_GetObjectItemCaseSensitive(j,"file");
    if (!cJSON_IsString(f) || !valid_name(f->valuestring)) {
      cJSON_Delete(j); return fail(req,"400 Bad Request","Карта недоступна или неверное имя WAV.");
    }
    snprintf(cmd.file,sizeof(cmd.file),"%s",f->valuestring);
  } else if (!strcmp(a->valuestring,"format")) {
    const cJSON *confirmation=cJSON_GetObjectItemCaseSensitive(j,"confirmation");
    if (!cJSON_IsString(confirmation) || strcmp(confirmation->valuestring,"ERASE_SD_CARD")) {
      cJSON_Delete(j); return fail(req,"400 Bad Request","Нужно подтвердить удаление всех файлов на SD-карте.");
    }
  } else if (!strcmp(a->valuestring,"volume")) {
    const cJSON *v=cJSON_GetObjectItemCaseSensitive(j,"value");
    if (!cJSON_IsNumber(v) || v->valuedouble<0 || v->valuedouble>100) {
      cJSON_Delete(j); return fail(req,"400 Bad Request","Громкость: от 0 до 100.");
    }
    cmd.volume=v->valueint;
  } else if (strcmp(a->valuestring,"pause") && strcmp(a->valuestring,"stop")) {
    cJSON_Delete(j); return fail(req,"400 Bad Request","Неизвестное действие.");
  }
  snprintf(cmd.action,sizeof(cmd.action),"%s",a->valuestring); cJSON_Delete(j);
  if (!strcmp(cmd.action,"format")) id=0;
  // Reserve formatting and enqueue under the same lock as player state updates.
  xSemaphoreTake(lock,portMAX_DELAY);
  const char *error=NULL;
  bool is_format=!strcmp(cmd.action,"format");
  if (formatting) error="Идёт форматирование. Дождитесь завершения.";
  else if (uploading) error="Идёт загрузка файла. Дождитесь завершения.";
  else if (is_format && !sd_ready) error="Карта не отвечает: форматирование недоступно.";
  else if (is_format && !all_players_idle())
    error="Сначала остановите обе линии и дождитесь выполнения команд. Паузы недостаточно.";
  else if (!strcmp(cmd.action,"play") && !mounted) error="Файловая система карты недоступна.";
  if (!error) {
    if (is_format) formatting=true;
    if (!xQueueSend(commands[id],&cmd,0)) {
      if (is_format) formatting=false;
      error="Плеер занят. Повторите команду.";
    } else if (is_format) {
      storage_error[0]=0; format_result[0]=0;
    }
  }
  xSemaphoreGive(lock);
  if (error) return fail(req,"409 Conflict",error);
  if (is_format) httpd_resp_set_status(req,"202 Accepted");
  cJSON *result=cJSON_CreateObject(); cJSON_AddBoolToObject(result,"ok",true);
  return json_reply(req,result);
}

static int hex_digit(char c) {
  if (c>='0' && c<='9') return c-'0';
  if (c>='a' && c<='f') return c-'a'+10;
  if (c>='A' && c<='F') return c-'A'+10;
  return -1;
}
static bool upload_name(const char *encoded,char *name,size_t capacity) {
  size_t out=0;
  for (size_t i=0;encoded[i];i++) {
    unsigned char c=encoded[i];
    if (c=='%') {
      if (!encoded[i+1] || !encoded[i+2]) return false;
      int hi=hex_digit(encoded[i+1]), lo=hex_digit(encoded[i+2]);
      if (hi<0 || lo<0) return false;
      c=(hi<<4)|lo; i+=2;
    }
    if (!c || out+1>=capacity) return false;
    name[out++]=c;
  }
  name[out]=0;
  return valid_name(name);
}
// Close rejected request bodies instead of draining a potentially large upload.
static esp_err_t upload_reject(httpd_req_t *req,const char *status,const char *msg) {
  httpd_resp_set_hdr(req,"Connection","close");
  fail(req,status,msg);
  return ESP_FAIL;
}
static esp_err_t upload_handler(httpd_req_t *req) {
  char encoded[768], name[256], path[264];
  if (!req->content_len || req->content_len>INT32_MAX)
    return upload_reject(req,"413 Content Too Large","Размер WAV должен быть от 1 байта до 2 ГБ.");
  if (httpd_req_get_hdr_value_str(req,"X-Filename",encoded,sizeof(encoded))!=ESP_OK ||
      !upload_name(encoded,name,sizeof(name)))
    return upload_reject(req,"400 Bad Request","Нужно корректное имя WAV без путей, длиной до 255 байт.");
  xSemaphoreTake(lock,portMAX_DELAY);
  bool available=mounted && !formatting && !uploading && all_players_idle();
  if (available) uploading=true;
  xSemaphoreGive(lock);
  if (!available)
    return upload_reject(req,"409 Conflict","Нужна доступная карта и остановленный плеер. Дождитесь других операций.");

  xSemaphoreTake(storage_lock,portMAX_DELAY);
  snprintf(path,sizeof(path),"/sd/%s",name);
  const char *error=NULL, *status="500 Internal Server Error";
  const char *temporary="/sd/.upload.tmp";
  bool temporary_created=false;
  struct stat st;
  FILE *file=NULL;
  ESP_LOGI(TAG,"Upload start: %s, %u bytes",name,(unsigned)req->content_len);
  if (!stat(path,&st)) {
    error="Файл с таким именем уже существует. Переименуйте загружаемый файл.";
    status="409 Conflict";
    goto done;
  }
  // Never expose a half-received WAV in the track list.
  file=fopen(temporary,"wb");
  if (!file) { error="Не удалось создать файл на карте."; goto done; }
  temporary_created=true;
  char buffer[4096];
  size_t received=0;
  while (received<req->content_len) {
    size_t size=req->content_len-received;
    if (size>sizeof(buffer)) size=sizeof(buffer);
    int n=httpd_req_recv(req,buffer,size);
    if (n<=0) {
      ESP_LOGE(TAG,"Upload receive failed: %d, received %u/%u",n,(unsigned)received,(unsigned)req->content_len);
      error="Загрузка прервана или истекло время ожидания."; status="408 Request Timeout"; goto done;
    }
    if (fwrite(buffer,1,n,file)!=(size_t)n) {
      error="Ошибка записи: проверьте свободное место и состояние карты."; goto done;
    }
    received+=n;
  }
  if (fclose(file)) { file=NULL; error="Не удалось сохранить файл на карте."; goto done; }
  file=fopen(temporary,"rb");
  wav_info info={0};
  const char *wav_error=file?read_wav(file,&info):"Не удалось проверить записанный WAV на карте.";
  if (wav_error) {
    error=wav_error;
    status="400 Bad Request"; goto done;
  }
  fclose(file); file=NULL;
  if (rename(temporary,path)) { error="Не удалось сохранить имя файла."; goto done; }
  temporary_created=false;
  if (stat(path,&st) || (size_t)st.st_size!=req->content_len) {
    error="Файл передан, но проверка записи на SD не прошла. Проверьте карту.";
    goto done;
  }
done:
  if (file) fclose(file);
  if (temporary_created && unlink(temporary))
    ESP_LOGW(TAG,"Could not remove incomplete upload");
  xSemaphoreTake(lock,portMAX_DELAY); uploading=false; xSemaphoreGive(lock);
  xSemaphoreGive(storage_lock);
  if (error) {
    ESP_LOGE(TAG,"Upload %s failed: %s",name,error);
    set_error(error);
    return upload_reject(req,status,error);
  }
  ESP_LOGI(TAG,"Upload saved and verified: %s, %u bytes",name,(unsigned)req->content_len);
  set_error("");
  cJSON *result=cJSON_CreateObject(); cJSON_AddBoolToObject(result,"ok",true);
  cJSON_AddStringToObject(result,"file",name);
  cJSON_AddNumberToObject(result,"bytes",req->content_len);
  httpd_resp_set_status(req,"201 Created");
  return json_reply(req,result);
}
extern const char page_start[] asm("_binary_index_html_start");
static esp_err_t root_handler(httpd_req_t *req) {
  httpd_resp_set_type(req,"text/html; charset=utf-8");
  httpd_resp_set_hdr(req,"Cache-Control","no-store");
  return httpd_resp_sendstr(req,page_start);
}

void app_main(void) {
  lock=xSemaphoreCreateMutex(); storage_lock=xSemaphoreCreateMutex();
  settings_lock=xSemaphoreCreateMutex();
  assert(lock && storage_lock && settings_lock);
  for (int i=0;i<LINE_COUNT;i++) {
    commands[i]=xQueueCreate(8,sizeof(command)); assert(commands[i]);
  }
  esp_err_t err=nvs_flash_init();
  if (err==ESP_ERR_NVS_NO_FREE_PAGES || err==ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase()); err=nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);
  load_settings();
  const int bclk[]={3,5}, ws[]={9,4}, dout[]={14,6};
  for (int i=0;i<LINE_COUNT;i++) {
    i2s_chan_config_t channel=I2S_CHANNEL_DEFAULT_CONFIG((i2s_port_t)i,I2S_ROLE_MASTER);
    channel.auto_clear=true;
    channel.dma_desc_num=6; channel.dma_frame_num=512;
    ESP_ERROR_CHECK(i2s_new_channel(&channel,&outputs[i],NULL));
    i2s_std_config_t cfg={
      .clk_cfg=I2S_STD_CLK_DEFAULT_CONFIG(44100),
      .slot_cfg=I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,I2S_SLOT_MODE_STEREO),
      .gpio_cfg={.mclk=I2S_GPIO_UNUSED,.bclk=bclk[i],.ws=ws[i],.dout=dout[i],.din=I2S_GPIO_UNUSED}
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(outputs[i],&cfg));
  }
  init_sd();
  for (int i=0;i<LINE_COUNT;i++) {
    if (mounted && players[i].autoplay) {
      command restore={.action="play",.start_paused=players[i].paused};
      snprintf(restore.file,sizeof(restore.file),"%s",players[i].track);
      assert(xQueueSend(commands[i],&restore,0)==pdPASS);
    }
    assert(xTaskCreate(player_task,i?"player2":"player1",12288,
                       (void *)(intptr_t)i,5,NULL)==pdPASS);
  }
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_ap();
  wifi_init_config_t wifi=WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&wifi));
  wifi_config_t ap={.ap={.ssid="Soundboard-SD",.password="soundboard123",
    .channel=6,.max_connection=4,.authmode=WIFI_AUTH_WPA2_PSK}};
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP,&ap));
  ESP_ERROR_CHECK(esp_wifi_start());
  httpd_config_t http=HTTPD_DEFAULT_CONFIG(); http.stack_size=12288;
  http.recv_wait_timeout=20;
  httpd_handle_t server;
  ESP_ERROR_CHECK(httpd_start(&server,&http));
  const httpd_uri_t routes[]={
    {.uri="/",.method=HTTP_GET,.handler=root_handler},
    {.uri="/api/status",.method=HTTP_GET,.handler=status_handler},
    {.uri="/api/files",.method=HTTP_GET,.handler=files_handler},
    {.uri="/api/control",.method=HTTP_POST,.handler=control_handler},
    {.uri="/api/upload",.method=HTTP_POST,.handler=upload_handler}
  };
  for(size_t i=0;i<sizeof(routes)/sizeof(routes[0]);i++)
    ESP_ERROR_CHECK(httpd_register_uri_handler(server,&routes[i]));
  ESP_LOGI(TAG,"Open http://192.168.4.1 — WiFi Soundboard-SD, PSRAM disabled");
}

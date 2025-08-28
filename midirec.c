#include <alsa/asoundlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <signal.h>

static volatile sig_atomic_t g_stop = 0;
void handle_sigint(int sig) { (void)sig; g_stop = 1; }

// Write variable-length quantity for MIDI delta time
void write_varlen(FILE *f, unsigned int value) {
    unsigned char buffer[4];
    int i = 0;
    buffer[i++] = value & 0x7F;
    while ((value >>= 7) > 0) {
        buffer[i++] = 0x80 | (value & 0x7F);
    }
    for (int j = i - 1; j >= 0; j--) fputc(buffer[j], f);
}

int main(void) {
    snd_rawmidi_t *midi_in = NULL;
    snd_ctl_t *ctl;
    snd_rawmidi_info_t *info;
    int card = -1, dev;
    char device_name[32];
    int found = 0;

    signal(SIGINT, handle_sigint);
    snd_rawmidi_info_alloca(&info);

    // --- Auto-detect first USB MIDI input ---
    while (snd_card_next(&card) >= 0 && card >= 0 && !found) {
        char ctl_name[32];
        snprintf(ctl_name, sizeof(ctl_name), "hw:%d", card);
        if (snd_ctl_open(&ctl, ctl_name, 0) < 0) continue;

        dev = -1;
        while (snd_ctl_rawmidi_next_device(ctl, &dev) >= 0 && dev >= 0 && !found) {
            snd_rawmidi_info_set_device(info, dev);
            snd_rawmidi_info_set_subdevice(info, 0);
            snd_rawmidi_info_set_stream(info, SND_RAWMIDI_STREAM_INPUT);
            if (snd_ctl_rawmidi_info(ctl, info) >= 0) {
                snprintf(device_name, sizeof(device_name), "hw:%d,%d", card, dev);
                if (snd_rawmidi_open(&midi_in, NULL, device_name, 0) >= 0) {
                    printf("Using MIDI input device: %s\n", device_name);
                    found = 1;
                    break;
                }
            }
        }
        snd_ctl_close(ctl);
    }

    if (!found) { fprintf(stderr,"No USB MIDI input found\n"); return 1; }

    FILE *midi_f = fopen("record.mid", "wb");
    if (!midi_f) { perror("Cannot create MIDI file"); return 1; }

    uint16_t ppq = 480; // ticks per quarter note

    // --- MIDI Header Chunk ---
    fputc('M', midi_f); fputc('T', midi_f); fputc('h', midi_f); fputc('d', midi_f);
    fputc(0x00, midi_f); fputc(0x00, midi_f); fputc(0x00, midi_f); fputc(0x06, midi_f); // header length
    fputc(0x00, midi_f); fputc(0x00, midi_f); // format 0
    fputc(0x00, midi_f); fputc(0x01, midi_f); // 1 track
    fputc((ppq >> 8) & 0xFF, midi_f); fputc(ppq & 0xFF, midi_f); // ticks per quarter note

    // --- Track Chunk ---
    fputc('M', midi_f); fputc('T', midi_f); fputc('r', midi_f); fputc('k', midi_f);
    long track_len_pos = ftell(midi_f);
    fputc(0x00, midi_f); fputc(0x00, midi_f); fputc(0x00, midi_f); fputc(0x00, midi_f); // placeholder length

    uint8_t buf[256], status_byte=0, data[2]; 
    int data_count=0;
    struct timespec start_time, last_time;
    clock_gettime(CLOCK_REALTIME, &start_time); last_time = start_time;

    printf("Recording... Press Ctrl+C to stop.\n");

    while (!g_stop) {
        int n = snd_rawmidi_read(midi_in, buf, sizeof(buf));
        if (n <= 0) continue;

        for (int i=0;i<n;i++) {
            uint8_t byte = buf[i];
            if (byte & 0x80) { status_byte=byte; data_count=0; continue; }
            if (data_count<2) data[data_count++] = byte;
            uint8_t type = status_byte & 0xF0;
            int needed = (type==0xC0||type==0xD0)?1:2;
            if (data_count==needed) {
                struct timespec now; clock_gettime(CLOCK_REALTIME,&now);
                long delta_ms = (now.tv_sec - last_time.tv_sec)*1000 + (now.tv_nsec - last_time.tv_nsec)/1000000;
                int ticks = delta_ms * ppq / 500; // 120 BPM = 500ms per quarter note

                // --- Write to MIDI file ---
                write_varlen(midi_f, ticks);
                if (type==0x90 && data[1]==0) type=0x80; // NoteOn vel 0 = NoteOff
                fputc(type | (status_byte & 0x0F), midi_f);
                fputc(data[0], midi_f);
                if (!(type==0xC0 || type==0xD0)) fputc(data[1], midi_f);

                // --- Print to terminal ---
                if (type == 0x90) {
                    printf("%ld.%03ld NoteOn ch=%d note=%d vel=%d\n",
                           now.tv_sec, now.tv_nsec/1000000,
                           (status_byte & 0x0F)+1, data[0], data[1]);
                } else if (type == 0x80) {
                    printf("%ld.%03ld NoteOff ch=%d note=%d vel=%d\n",
                           now.tv_sec, now.tv_nsec/1000000,
                           (status_byte & 0x0F)+1, data[0], data[1]);
                } else if (type == 0xB0) {
                    printf("%ld.%03ld CC ch=%d cc=%d val=%d\n",
                           now.tv_sec, now.tv_nsec/1000000,
                           (status_byte & 0x0F)+1, data[0], data[1]);
                }

                last_time = now;
                data_count = 0;
            }
        }
    }

    // --- End of Track ---
    write_varlen(midi_f,0);
    fputc(0xFF, midi_f); fputc(0x2F, midi_f); fputc(0x00, midi_f);

    // --- Update track length ---
    long end_pos = ftell(midi_f);
    fseek(midi_f, track_len_pos, SEEK_SET);
    uint32_t track_len = end_pos - track_len_pos - 4;
    fputc((track_len >> 24) & 0xFF, midi_f);
    fputc((track_len >> 16) & 0xFF, midi_f);
    fputc((track_len >> 8) & 0xFF, midi_f);
    fputc(track_len & 0xFF, midi_f);

    fclose(midi_f);
    snd_rawmidi_close(midi_in);
    printf("Recording stopped. MIDI saved to record.mid\n");
    return 0;
}


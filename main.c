#include <libusb-1.0/libusb.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>

#define VENDOR_ID  0xfc02
#define PRODUCT_ID 0x0101
#define IFACE_NUM  1
#define EP_IN      0x81
#define EP_OUT     0x02

static volatile sig_atomic_t g_stop = 0;
void handle_sigint(int sig) { (void)sig; g_stop = 1; }

// --- Write MIDI variable-length quantity ---
void write_varlen(FILE *f, unsigned int value) {
    unsigned char buffer[4];
    int i = 0;
    buffer[i++] = value & 0x7F;
    while ((value >>= 7) > 0) {
        buffer[i++] = 0x80 | (value & 0x7F);
    }
    for (int j = i - 1; j >= 0; j--) fputc(buffer[j], f);
}

// --- Read variable-length quantity ---
unsigned int read_varlen(FILE *f) {
    unsigned int value = 0;
    int c;
    do {
        c = fgetc(f);
        if (c == EOF) return 0;
        value = (value << 7) | (c & 0x7F);
    } while (c & 0x80);
    return value;
}

// --- Record mode ---
int do_record(libusb_device_handle *devh, char* file) {
    FILE *midi_f = fopen(file, "wb");
    if (!midi_f) { perror("Cannot create MIDI file"); return 1; }

    uint16_t ppq = 480;

    // --- Header ---
    fputs("MThd", midi_f);
    fputc(0, midi_f); fputc(0, midi_f); fputc(0, midi_f); fputc(6, midi_f);
    fputc(0, midi_f); fputc(0, midi_f);
    fputc(0, midi_f); fputc(1, midi_f);
    fputc((ppq>>8)&0xFF, midi_f); fputc(ppq&0xFF, midi_f);

    // --- Track ---
    fputs("MTrk", midi_f);
    long track_len_pos = ftell(midi_f);
    fputc(0, midi_f); fputc(0, midi_f); fputc(0, midi_f); fputc(0, midi_f);

    // --- Setup variables ---
    int first_note_played = 0;
    struct timespec start, last;

    printf("Recording... Press Ctrl+C to stop.\n");

    while (!g_stop) {
        uint8_t buf[64];
        int transferred;
        int r = libusb_bulk_transfer(devh, EP_IN, buf, sizeof(buf), &transferred, 1000);
        if (r == 0 && transferred >= 4) {
            for (int i = 0; i < transferred; i += 4) {
                uint8_t cin = buf[i] & 0x0F;
                uint8_t s   = buf[i+1];
                uint8_t d1  = buf[i+2];
                uint8_t d2  = buf[i+3];

                if (cin >= 0x8 && cin <= 0xE) {
                    // Start timing on first NoteOn
                    if (!first_note_played && (s & 0xF0) == 0x90 && d2 > 0) {
                        clock_gettime(CLOCK_REALTIME, &start);
                        last = start;
                        first_note_played = 1;

                        // Optional: write tempo meta event just before first note
                        write_varlen(midi_f, 0);
                        fputc(0xFF, midi_f); fputc(0x51, midi_f); fputc(0x03, midi_f);
                        fputc(0x07, midi_f); fputc(0xA1, midi_f); fputc(0x20, midi_f);
                    }

                    // Skip events until first note
                    if (!first_note_played) continue;

                    struct timespec now;
                    clock_gettime(CLOCK_REALTIME,&now);
                    long delta_ms = (now.tv_sec - last.tv_sec) * 1000 +
                                    (now.tv_nsec - last.tv_nsec) / 1000000;
                    int ticks = delta_ms * ppq / 500; // 120 BPM

                    write_varlen(midi_f, ticks);
                    fputc(s, midi_f);
                    fputc(d1, midi_f);
                    if (!(s >= 0xC0 && s <= 0xDF)) fputc(d2, midi_f);

                    last = now;

                    // Terminal output
                    if ((s & 0xF0)==0x90 && d2>0)
                        printf("NoteOn ch=%d note=%d vel=%d\n",(s&0x0F)+1,d1,d2);
                    else if ((s & 0xF0)==0x80 || ((s & 0xF0)==0x90 && d2==0))
                        printf("NoteOff ch=%d note=%d vel=%d\n",(s&0x0F)+1,d1,d2);
                }
            }
        }
    }

    // --- End of track ---
    write_varlen(midi_f, 0);
    fputc(0xFF, midi_f); fputc(0x2F, midi_f); fputc(0x00, midi_f);

    // Fix track length
    long end_pos = ftell(midi_f);
    fseek(midi_f, track_len_pos, SEEK_SET);
    uint32_t len = end_pos - track_len_pos - 4;
    fputc((len>>24)&0xFF,midi_f);
    fputc((len>>16)&0xFF,midi_f);
    fputc((len>>8)&0xFF,midi_f);
    fputc(len&0xFF,midi_f);

    fclose(midi_f);
    printf("Recording stopped. Saved to record.mid\n");
    return 0;
}

// --- Playback mode ---
int do_play(libusb_device_handle *devh, char* file, bool loop) {
    FILE *midi_f = fopen(file, "rb");
    if (!midi_f) { perror("Cannot open record.mid"); return 1; }

    uint16_t ppq = 480;

    printf("Playing back record.mid ... Ctrl+C to stop.\n");

    while (!g_stop) {
        // reset to start of track
        fseek(midi_f, 14, SEEK_SET); // after MThd
        fseek(midi_f, 8, SEEK_CUR);  // after MTrk + track length

        while (!g_stop) {
            unsigned int delta = read_varlen(midi_f);
            if (delta > 0) {
                int ms = delta * 500 / ppq;  // 120 BPM fixed
                struct timespec ts = { ms/1000, (ms%1000)*1000000 };
                nanosleep(&ts, NULL);
            }

            int status = fgetc(midi_f);
            if (status == EOF) break;
            if (status == 0xFF) { // meta
                int meta = fgetc(midi_f);
                int len  = fgetc(midi_f);
                if (meta == 0x2F) { // end of track
                    goto track_end;
                }
                fseek(midi_f, len, SEEK_CUR);
                continue;
            }

            uint8_t msg[4] = {0};
            msg[1] = status;
            if ((status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0) {
                msg[2] = fgetc(midi_f);
            } else {
                msg[2] = fgetc(midi_f);
                msg[3] = fgetc(midi_f);
            }

            uint8_t cin = (status >> 4);
            uint8_t usb_pkt[4] = { cin, msg[1], msg[2], msg[3] };

            int transferred;
            libusb_bulk_transfer(devh, EP_OUT, usb_pkt, 4, &transferred, 1000);

            printf("Sent MIDI status=0x%02X d1=%d d2=%d\n", msg[1], msg[2], msg[3]);
        }

    track_end:
        if (!loop || g_stop) break;

        struct timespec ts = {1, 0};  // 1 second, 0 nanoseconds
        nanosleep(&ts, NULL);

        rewind(midi_f); // reset file for next iteration
    }

    fclose(midi_f);
    printf("Playback finished.\n");
    return 0;
}

// --- Main ---
int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s [-rec | -play | -playloop] file\n", argv[0]);
        return 1;
    }

    libusb_context *ctx;
    libusb_device_handle *devh = NULL;
    signal(SIGINT, handle_sigint);

    if (libusb_init(&ctx) < 0) { fprintf(stderr, "libusb init failed\n"); return 1; }

    devh = libusb_open_device_with_vid_pid(ctx, VENDOR_ID, PRODUCT_ID);
    if (!devh) { fprintf(stderr, "Cannot open USB MIDI device %04x:%04x\n", VENDOR_ID, PRODUCT_ID); libusb_exit(ctx); return 1; }

    if (libusb_kernel_driver_active(devh, IFACE_NUM) == 1)
        libusb_detach_kernel_driver(devh, IFACE_NUM);

    if (libusb_claim_interface(devh, IFACE_NUM) < 0) {
        fprintf(stderr, "Cannot claim interface %d\n", IFACE_NUM);
        libusb_close(devh); libusb_exit(ctx); return 1;
    }

    int rc = 0;
    if (strcmp(argv[1], "-rec") == 0) {
        rc = do_record(devh, argv[2]);
    } else if (strcmp(argv[1], "-play") == 0) {
        rc = do_play(devh, argv[2], false);
    } else if (strcmp(argv[1], "-playloop") == 0) {
        rc = do_play(devh, argv[2], true);
    } else {
        fprintf(stderr, "Unknown option: %s\n", argv[1]);
        rc = 1;
    }

    libusb_release_interface(devh, IFACE_NUM);
    libusb_close(devh);
    libusb_exit(ctx);
    return rc;
}


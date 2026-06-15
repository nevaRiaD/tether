#ifndef AUDIO_H
#define AUDIO_H

/**
 * @brief Mount the "audio" LittleFS partition and initialise the codec/player.
 *
 * On failure the partition mount is logged and the function returns early;
 * codec/player init results are logged but not treated as fatal.
 */
void audio_init(void);

#endif // AUDIO_H

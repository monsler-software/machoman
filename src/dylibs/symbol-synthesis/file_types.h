#ifndef MACHOMAN_FILE_TYPES_H
#define MACHOMAN_FILE_TYPES_H

#include <string_view>

/* AVFoundation's file type constants are UTI identifiers, rather than
 * opaque keys. CoreServices uses the same identifiers for descriptions. */
struct mm_av_file_type {
    std::string_view symbol, identifier, description, parent;
};
inline constexpr mm_av_file_type mm_av_file_types[] = {
    {"AVFileTypeQuickTimeMovie", "com.apple.quicktime-movie", "QuickTime movie", "public.movie"},
    {"AVFileTypeMPEG4", "public.mpeg-4", "MPEG-4 movie", "public.movie"},
    {"AVFileTypeAppleM4V", "com.apple.m4v-video", "MPEG-4 video", "public.mpeg-4"},
    {"AVFileTypeAppleM4A", "com.apple.m4a-audio", "MPEG-4 audio", "public.audio"},
    {"AVFileType3GPP", "public.3gpp", "3GPP movie", "public.movie"},
    {"AVFileType3GPP2", "public.3gpp2", "3GPP2 movie", "public.movie"},
    {"AVFileTypeCoreAudioFormat", "com.apple.coreaudio-format", "Core Audio format", "public.audio"},
    {"AVFileTypeWAVE", "com.microsoft.waveform-audio", "Waveform audio", "public.audio"},
    {"AVFileTypeAIFF", "public.aiff-audio", "AIFF audio", "public.aifc-audio"},
    {"AVFileTypeAIFC", "public.aifc-audio", "AIFC audio", "public.audio"},
    {"AVFileTypeAMR", "org.3gpp.adaptive-multi-rate-audio", "AMR audio", "public.audio"},
    {"AVFileTypeMPEGLayer3", "public.mp3", "MP3 audio", "public.audio"},
    {"AVFileTypeAC3", "public.ac3-audio", "AC-3 audio", "public.audio"},
    {"AVFileTypeEnhancedAC3", "public.enhanced-ac3-audio", "Enhanced AC-3 audio", "public.audio"},
    {"AVFileTypeJPEG", "public.jpeg", "JPEG image", "public.image"},
    {"AVFileTypeDNG", "com.adobe.raw-image", "Digital Negative image", "public.camera-raw-image"},
    {"AVFileTypeHEIC", "public.heic", "HEIF image", "public.heif"},
    {"AVFileTypeAVCI", "public.avci", "AVC image", "public.image"},
    {"AVFileTypeHEIF", "public.heif", "HEIF image", "public.image"},
    {"AVFileTypeTIFF", "public.tiff", "TIFF image", "public.image"},
};

#endif

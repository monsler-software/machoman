#ifndef MACHOMAN_FILE_TYPES_H
#define MACHOMAN_FILE_TYPES_H

#include <string_view>

/* AVFoundation's file type constants are UTI identifiers, rather than
 * opaque keys. CoreServices uses the same identifiers for descriptions. */
struct mm_av_file_type {
    std::string_view symbol, identifier, description;
};
inline constexpr mm_av_file_type mm_av_file_types[] = {
    {"AVFileTypeQuickTimeMovie", "com.apple.quicktime-movie", "QuickTime movie"},
    {"AVFileTypeMPEG4", "public.mpeg-4", "MPEG-4 movie"},
    {"AVFileTypeAppleM4V", "com.apple.m4v-video", "MPEG-4 video"},
    {"AVFileTypeAppleM4A", "com.apple.m4a-audio", "MPEG-4 audio"},
    {"AVFileType3GPP", "public.3gpp", "3GPP movie"},
    {"AVFileType3GPP2", "public.3gpp2", "3GPP2 movie"},
    {"AVFileTypeCoreAudioFormat", "com.apple.coreaudio-format", "Core Audio format"},
    {"AVFileTypeWAVE", "com.microsoft.waveform-audio", "Waveform audio"},
    {"AVFileTypeAIFF", "public.aiff-audio", "AIFF audio"},
    {"AVFileTypeAIFC", "public.aifc-audio", "AIFC audio"},
    {"AVFileTypeAMR", "org.3gpp.adaptive-multi-rate-audio", "AMR audio"},
    {"AVFileTypeMPEGLayer3", "public.mp3", "MP3 audio"},
    {"AVFileTypeAC3", "public.ac3-audio", "AC-3 audio"},
    {"AVFileTypeEnhancedAC3", "public.enhanced-ac3-audio", "Enhanced AC-3 audio"},
    {"AVFileTypeJPEG", "public.jpeg", "JPEG image"},
    {"AVFileTypeDNG", "com.adobe.raw-image", "Digital Negative image"},
    {"AVFileTypeHEIC", "public.heic", "HEIF image"},
    {"AVFileTypeAVCI", "public.avci", "AVC image"},
    {"AVFileTypeHEIF", "public.heif", "HEIF image"},
    {"AVFileTypeTIFF", "public.tiff", "TIFF image"},
};

#endif

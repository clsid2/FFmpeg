LIBAVCODEC_MAJOR {
    global:
        av*;
        #deprecated, remove after next bump
        audio_resample;
        audio_resample_close;
        #LAV usage
        ff_vc1_pixel_aspect;
        ff_crop_tab;
        ff_flac_is_extradata_valid;
    local:
        *;
};

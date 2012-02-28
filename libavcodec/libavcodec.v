LIBAVCODEC_MAJOR {
    global:
        av_*;
        avcodec_*;
        avpriv_*;
        avsubtitle_free;
        #LAV usage
        ff_vc1_pixel_aspect;
        ff_crop_tab;
        ff_flac_is_extradata_valid;
    local:
        *;
};

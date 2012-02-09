LIBAVFORMAT_MAJOR {
    global:
        av*;
        #for LAV Audio/Video
        ff_sipr_subpk_size;
        ff_rm_reorder_sipr_data;
        ff_vorbis_comment;
    local:
        *;
};

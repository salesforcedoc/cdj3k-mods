# EP122 symbol spec - the source of truth for every address the shim needs.
#
# EP122 ships stripped, so the shim used to carry ~190 hardcoded addresses, each
# one valid for exactly one firmware build. This file names what we need instead
# of where it lives, and scripts/gen-ep122-syms.py turns it into
# mods/ep122_syms.h, which the runtime resolver (mods/resolve.c) consumes.
#
# Regenerate after touching this file:
#     scripts/gen-ep122-syms.py
#
# The generator RESOLVES every entry against build/EP122-deck and refuses to
# emit anything ambiguous, so a bad spec line fails on the Mac rather than on
# the deck. The address it resolved to is written into the header as a comment
# and re-checked on the next run: a silent change is a diff, not a surprise.
#
# ---- entry kinds ------------------------------------------------------------
#
#   vtable <NAME> class=<mangled> [base=<mangled>]
#       An RTTI address point. `class` is the RAW Itanium mangled name as it
#       appears in the binary (N3gui11UtilityViewE), not a demangled one -- the
#       mangling is what is actually stored, and matching it is exact. `base`
#       names a secondary vtable by the base class it serves, which survives a
#       layout change that would move offset-to-top.
#
#       `base` handles a VIRTUAL base too, which is what every gui:: widget
#       needs: juce::Component is virtual in gui::WidgetBase, so the class's
#       primary vtable is NOT the one carrying mouseDown/paint. The generator
#       reads the subobject's real offset out of the vtable rather than out of
#       the RTTI record, which for a virtual base holds no offset at all.
#
#   slot <NAME> vtable=<NAME> off=<hex>
#       A virtual function, resolved by READING the slot at run time. There is
#       no address to record and nothing to match: whatever the slot holds is by
#       definition that class's implementation. This is why the old *_FN_*
#       constants disappear rather than getting signatures -- they only ever
#       existed as mod_patch_slot's expect_fn post-condition, and a slot read
#       makes the post-condition trivially true.
#
#   func <NAME> addr=<hex>
#       A free function, resolved by a masked byte signature. The generator
#       grows the signature until it is unique across .text and records the
#       length it needed.
#
#   func <NAME> addr=<hex> in=<NAME> span=<hex> nth=<n>
#       Same, for a signature that is NOT unique: scope it to the BL targets of
#       an enclosing function and take the nth match. For a run of sibling
#       functions that differ only in the global they touch -- which is exactly
#       what the signature masks out -- position in the dispatch IS the
#       identity. The generator records how many matches it saw; a firmware that
#       changes the count fails the check instead of picking the wrong sibling.
#
#   data <NAME> addr=<hex> from=<NAME> insn=<n>
#       A global, resolved from the ADRP/ADD (or ADRP/LDR) pair at instruction
#       `insn` of an already-resolved function. The pair encodes the address the
#       code itself computes, so it moves with the data rather than with us.
#
#   capture <NAME>
#       Documented as deliberately NOT resolved statically: the value is taken
#       from a live call at run time. Listed here so the inventory is complete.

# =============================================================================
# vtables
# =============================================================================

# --- mods/stem/ui/ ---
vtable BTN                class=N3gui18TogglesImageButtonE
vtable TOUCHARIA          class=N3gui23WaveformViewTitleWidget9TouchAriaE
vtable DISPLAY_REFRESH    class=N3gui24DisplayRefleshCycleTimerE
vtable LABEL              class=N4juce5LabelE

# --- mods/browse/ ---
# The browse header, and the row a finger lands on. Both are asked for as the
# juce::Component they are: BROWSE_TITLE inherits Component virtually through
# gui::WidgetBase, so its primary vtable holds the widget's own interface and
# the Component slots live in a second one. RowComp's Component IS its primary.
vtable BROWSE_TITLE       class=N3gui17BrowseTitleWidgetE base=N4juce9ComponentE
vtable BROWSE_SWITCHES    class=N3gui29BrowseDispSwitchButtonsWidgetE
vtable TRACKLIST          class=N3gui15TrackListWidgetE base=N4juce9ComponentE
vtable TRACKLIST_HEADER   class=N3gui15TrackListHeaderE
vtable ROWCOMP            class=N4meow21TouchableTableListBox7RowCompE
# The per-row WRAPPER. A RowComp fills it at {0,0} and the wrapper is what
# carries the row's position, so the wrapper is what a drag moves -- moving the
# RowComp only slides it inside its own clip.
vtable LISTBOX_ROW        class=N4meow16TouchableListBox3RowE
# What the rows sit ON. A carried row leaves a hole, and the hole shows this
# component's own background, which is not the list's row colour.
vtable VIEWED_COMP        class=N4meow17TouchableViewport15ViewedComponentE
# The screen behind the deck's own PLAYLIST button. A track list under one of
# these is a playlist BY CONSTRUCTION -- that screen shows nothing else -- which
# is the half of the reorder's gate the browse sidebar cannot answer, because
# that screen has no sidebar of its own.
vtable PLAYLIST_VIEW      class=N3gui12PlayListViewE
# music_library::TrackListCondition -- the shape a list was ASKED FOR with, kept
# by the cache that answered. The UI knows a hierarchy and never a table key, but
# the collector keys every cached list by one of these, so the playlist id the
# query carried is still there long after the query itself is gone. Compared
# against a condition's vptr, the same identity test the deck's own predicates
# reach by __dynamic_cast.
vtable TRACK_LIST_CONDITION class=N13music_library18TrackListConditionE

# --- mods/mod_menu.c ---
vtable DJSET_MODEL        class=N3gui19DJSettingTableModelE
vtable DJSET_RMODEL       class=N3gui28DJSettingRightPaneTableModelE
vtable UTILITY_VIEW       class=N3gui11UtilityViewE
vtable SOURCE_SELECT_VIEW class=N3gui16SourceSelectViewE
vtable UTILITY_AS_MODEL_LISTENER class=N3gui11UtilityViewE base=N3gui19DJSettingTableModel9IListenerE
vtable UTILITY_AS_KBD_LISTENER   class=N3gui11UtilityViewE base=N3gui27SoftwareKeyboardPopupWidget9IListenerE

# --- mods/theme.c ---
vtable GFX_RENDERER       class=N4juce32LowLevelGraphicsSoftwareRendererE
vtable SOFT_PIXEL_DATA    class=N4juce17SoftwarePixelDataE
# The overview waveform IS one of these -- all three OverviewWaveform*Widget classes
# draw nothing themselves (gui::WidgetBase' draw slot is the no-op default) and hand
# the job to an embedded juce::ImageComponent. Its Component subobject is the base
# vtable, which is where paint lives.
vtable JUCE_IMAGECOMPONENT class=N4juce14ImageComponentE base=N4juce9ComponentE

# The overview waveform is BAKED, not painted: on the database reply thread, once per
# track, each of these renders the point data into an offscreen juce::Image which the
# widget then simply blits. That is why no amount of paint-time probing ever found it --
# there is no repaint to catch. One class per WAVEFORM COLOR mode, which is also why
# 3BAND themes and BLUE/RGB do not: three different bakers.
vtable WAVEREPLY_1200PT   class=N15db_access_proxy22WaveformReplyer_1200PtE
vtable WAVEREPLY_400PT    class=N15db_access_proxy21WaveformReplyer_400PtE
vtable WAVEREPLY_3BAND    class=N15db_access_proxy21WaveformReplyer_3BandE
# The waveform request path, used only as an anchor: its one virtual method is
# where ASYNC_REQUEST_ID is separated from its twin, on both SoCs. The class has a
# single vtable at offset-to-top 0 in every extracted build, and +0x10 is
# its LAST code slot -- the next qword is the following group's offset-to-top --
# so nothing declared earlier can shift it.
vtable WAVEFORM_REQ_THROWER class=N15db_access_proxy22WaveformRequestThrowerE

# --- mods/stem/audio.c: the pcmbuf read() probes ---
vtable PCM_THRU           class=N6pcmbuf22ThruSamplingRateBufferE
vtable PCM_SEQ            class=N6pcmbuf9SeqBufferE
vtable PCM_SIMPLE         class=N6pcmbuf12SimpleBufferE
vtable PCM_PAGEBUF        class=N6pcmbuf7pagebuf10PageBufferINS0_23DefaultPageStaticConfigEEE
vtable PCM_STRETCH        class=N11time_domain26ReadableTimeStretchAdapterE
vtable PCM_PREVIEW        class=N14preview_player17RealtimeSRCBufferE
vtable TSMGR              class=N11time_domain26CascadedTimeStretchManagerE
# The deck's own "the track is in the pool" event: onLoadResult posts this task
# with the load's SourceId at closure+0x18/+0x20 (the same 16 bytes the pool
# reads carry) and its Result at +0x30. Fires whether or not anything plays,
# which the reads do not on a deck loaded and left at 0:00.
vtable PCM_LOADRESULT_TASK class=*N4meow16AsyncTaskBoxBase9AsyncTaskIZN9dj_player24PcmBufferFunctionHandler12onLoadResultEN6pcmbuf19ILoadResultListener6ResultERKN7trackid7TrackIDERKNS_18RefCountedObjExPtrINS4_10SourceInfoEEEN12audio_format15AudioFormatKindEEUlvE_EE

# --- mods/stem/decode.c ---
vtable SRC                class=N12audio_format19SampleRateConverterE

# One reader and one factory per container. They dispatch on the path's
# extension, so the first factory that recognises a name wins and the reader it
# builds is the matching one; we hook both sides of every container rather than
# guess which the deck will pick.
# The singleton every reader is created through. Its vtable is here so the
# pointer we capture from a live call can be checked against a class rather than
# taken on trust.
vtable AUDIO_READER_FACTORY class=N12audio_format18AudioReaderFactoryE

vtable READER_FLAC        class=N12audio_format12FileReadFlacE
vtable READER_MP3         class=N12audio_format11FileReadMp3E
vtable READER_AAC         class=N12audio_format11FileReadAacE
vtable READER_ALAC        class=N12audio_format12FileReadAlacE
vtable READER_MP4         class=N12audio_format11FileReadMp4E
vtable READER_AIFF        class=N12audio_format12FileReadAiffE
vtable READER_WAV         class=N12audio_format11FileReadWavE
# The fallback behind the seven: what AudioReaderFactory builds when no native
# factory takes a file -- a 32-bit float WAV, which FileReadWav refuses and the
# deck still plays. Same AbstractReader interface, same open() slot, so the
# capture sees the open the deck actually succeeded with.
vtable READER_JUCE        class=N12audio_format22JuceAudioReaderWrapperE
vtable FACTORY_FLAC       class=N12audio_format19FileReadFlacFactoryE
vtable FACTORY_MP3        class=N12audio_format18FileReadMp3FactoryE
vtable FACTORY_AAC        class=N12audio_format18FileReadAacFactoryE
vtable FACTORY_ALAC       class=N12audio_format19FileReadAlacFactoryE
vtable FACTORY_MP4        class=N12audio_format18FileReadMp4FactoryE
vtable FACTORY_AIFF       class=N12audio_format19FileReadAiffFactoryE
vtable FACTORY_WAV        class=N12audio_format18FileReadWavFactoryE

# open() and getSampleRate() are the same slots in every reader, so one pair of
# names covers all seven; FLAC is the one we build ourselves and therefore the
# one whose identity is checked before +0x88 is read as a rate.
slot READER_FLAC_OPEN     vtable=READER_FLAC        off=0x10
slot READER_FLAC_GETRATE  vtable=READER_FLAC        off=0x50
# The converter names its own destructor: slot 0 of a vtable is the complete
# object dtor. A class name and an offset are source, so this resolves on any
# build and either SoC, which a byte signature does not.
slot SRC_DTOR             vtable=SRC                off=0x0
slot SRC_SETSOURCE        vtable=SRC                off=0x10
slot SRC_FILEREAD         vtable=SRC                off=0x18

# --- mods/wave_*.c ---
vtable WAVE_ANALYZE_CTRL  class=N7usecase14track_analysis32UsecaseWaveformAnalyzeControllerE
vtable WAVE_REQ_HANDLER   class=N17trackinfo_stocker30DetailedWaveformRequestHandlerIN14appnd_trk_info22DetailedWaveform_3BandEEE
vtable WAVE_RECEPTION     class=N17trackinfo_stocker30DetailedWaveformRequestHandlerIN14appnd_trk_info22DetailedWaveform_3BandEE9ReceptionE
# The other two waveform flavours. Same template, different payload class, and
# each one's reply lands on a different slot because the Reception interface is
# reached through a different base -- hence three names rather than an index.
# The beat grid arriving for a track, which is the only route to one that does
# not go through a cue slot -- a track with no cues has no reachable grid at all.
# Slot 3 is replyBeatGridRequest(RequestID&, TrackID&, SharedBeatGridPtr&,
# AsyncCommand::RequestResult&); the binary's own assert string names it, and its
# forward (x(listener, *(this+8), arg3, arg4, *arg5)) fixes the registers:
# x2 = TrackID, x3 = the grid.
vtable GRID_RECEPTION      class=N17trackinfo_stocker22BeatGridRequestHandler9ReceptionE
vtable WAVE_RECEPTION_RGB  class=N17trackinfo_stocker30DetailedWaveformRequestHandlerIN14appnd_trk_info20DetailedWaveform_RGBEE9ReceptionE
vtable WAVE_RECEPTION_BLUE class=N17trackinfo_stocker30DetailedWaveformRequestHandlerIN14appnd_trk_info21DetailedWaveform_BlueEE9ReceptionE
# Where the waveform's colours come from. Slot +0x30 answers one column and band
# with {height, B, G, R, A}, and renderWaveform_new lerps between the columns that
# fall under an output pixel. A band a column does not have comes back as a
# default-constructed juce::Colour -- transparent black -- which is the only black
# in the picture and the whole of the dark contour on a light theme.
# One vtable per WAVEFORM COLOR mode. The 3Band one drives both the 1280x190
# detailed strip and the 1020x43 overview: two instances of a single widget.
vtable WAVE_PROVIDER_3BAND class=N3gui8waveform35DetailedWaveformLayeredDataProviderIN14appnd_trk_info22DetailedWaveform_3BandEEE
vtable WAVE_PROVIDER_RGB   class=N3gui8waveform28DetailedWaveformDataProviderIN14appnd_trk_info20DetailedWaveform_RGBEEE
vtable WAVE_PROVIDER_BLUE  class=N3gui8waveform28DetailedWaveformDataProviderIN14appnd_trk_info21DetailedWaveform_BlueEEE

# The repository cache, which is where a beat grid is REGISTERED -- the deck's
# own way to keep one, ending in the DB server writing the Quantize atom into
# the track's analysis file. Slot 0x50 takes a whole grid, slot 0x58 an int16
# offset; the second is what the deck's own GRID ADJUST uses, and its handler
# below is where every argument of the first was read from.
vtable TIR_CACHE           class=N21track_info_repository24TrackInfoRepositoryCacheE
vtable BEATGRID_OFF_REGIST class=N17trackinfo_stocker27BeatGridOffsetRegistHandlerE

# The DB engine's own file handle, and the reason mods/db/pdbwatch.c could watch
# the library files move and never see an analysis file: this one is STDIO.
# It opens with fopen (mode table at 0x2072518: "a", "ab", "r") and writes with
# fwrite, and glibc reaches open64/write from inside those -- binding within
# libc rather than through EP122's PLT, where an LD_PRELOAD would see them. So
# the watch is taken here, on the deck's own handle, which also names the caller
# in the DB layer rather than a libc frame.
vtable DB_FPHANDLE         class=N21track_info_repository9CFpHandleE

# --- mods/gate_cue.c: the AsyncTask closure types the deck posts pad work through ---
vtable GATE_PRESSPAD_TASK   class=N4meow16AsyncTaskBoxBase9AsyncTaskIZN13input_devices23DeckOperationPadHandlerINS2_15HotCueOperationEE8pressPadERKS4_RKNS2_23DeckInputDevicePriorityEEUlvE_EE
vtable GATE_RELEASEPAD_TASK class=N4meow16AsyncTaskBoxBase9AsyncTaskIZN13input_devices23DeckOperationPadHandlerINS2_15HotCueOperationEE10releasePadERKS4_RKNS2_23DeckInputDevicePriorityEEUlvE_EE
vtable GATE_PLAYPAUSE_TASK  class=*N4meow16AsyncTaskBoxBase9AsyncTaskIZN13input_devices16PlayPauseHandler6pushOnEvEUlvE_EE
# Back-cue is the same pad handler's releaseOperation closure -- a third task
# type alongside the two above, and the run body IS what a signature was
# matching before.
vtable GATE_RELEASEOP_TASK  class=N4meow16AsyncTaskBoxBase9AsyncTaskIZN13input_devices23DeckOperationPadHandlerINS2_15HotCueOperationEE16releaseOperationERKS4_RKNS2_23DeckInputDevicePriorityEEUlvE_EE
# The handler the pad closures point back at. Its slot +0x20 is read as a
# sentinel: back-cue is only driven inline when the live handler dispatches the
# same way this class does, which is how we know the closure layout applies.
# The CALL/DELETE button's own action, as the deck models it:
# input_devices::MemoryCueHandler::deleteCue(TriggerType, DeckInputDevicePriority&).
# TriggerType is what a momentary modifier needs -- press and release both come
# through here, so the closure carries which.
vtable GATE_HOTCUE_HANDLER  class=N13input_devices13HotCueHandlerE
# The deck's cue use-case, reached from the pad handler's deck resolver. Its
# slots forward to dj_player::ICueLoopSetter, one method per slot.
vtable CUE_CONTROLLER       class=N7usecase4deck13CueControllerE

# --- mods/gate_ui.c: the play screen's bottom rack and the two widgets the
# GATE CUE shortcut is measured against.
#
# These are the PRIMARY vtables, and gate_ui uses them only to read the typeinfo
# word beside each. It cannot use them to call or patch: every one of these
# classes is an UpdaterComponent model first and a juce::Component second, so the
# nine slots here are a listener interface and the Component lives at a secondary
# vtable. `base=` cannot name that one -- juce::Component is a VIRTUAL base, so
# its RTTI offset is not its layout offset -- but every vtable in a class's group
# carries the same typeinfo, and matching that identifies a subobject pointer
# whichever vtable it happens to point at.
# --- mods/gate_cue.c: the preview needle. PreviewController is the UI's way in
# to the green needle -- slot +0x10 carries the touched position as a NORMALISED
# float and slot +0x18 clears it, so the pair is press-and-release of the preview
# zone without hooking any of the strip's own drawing.
vtable PREVIEW_CTL          class=N7usecase4deck17PreviewControllerE

vtable PLAYERINFO_RACK      class=N3gui22NormalPlayerInfoWidgetE
vtable PLAYERINFO_TIME      class=N3gui20PlayerInfoTimeWidgetE
vtable PLAYERINFO_TEMPO     class=N3gui21PlayerInfoTempoWidgetE

# The RGB half of the panel's lamps. hui::HuiIndicatorAbs delegates its update to
# an UpdateBehavior at +0x10, of which there are two: SingleColor for a lamp that
# is only on or off, MultiColor for one with a colour -- which is what the hot
# cue pads are. Its update reads *(this+8) vtable +0x18 for a packed u64 (byte 0
# the LED index, bytes 4-6 R/G/B) and hands the pair to HUI_LED_WRITE.
#
# So one wrapper here sees every coloured lamp the app draws, with the index that
# says which, and needs nothing from the indicator that owns it.
vtable HUI_MULTICOLOR       class=N3hui15HuiIndicatorAbs10MultiColorE

# WHICH lamp a MultiColor is looking at. Its *(this+8) is the lamp's colour
# source, and that source is one of exactly five classes on every build -- the
# four indicator_control::IndicatorWithIdentifier<K> instantiations and the
# indicator_control::Indicator they share getColor() with. So the source's own
# vtable names the lamp's family, and this one is the deck's.
#
# indicator_control::DeckIndicatorController builds all 35 deck sources in one
# loop, `for i in 0 .. 0x22`: 0x70 bytes each, vtable at +0x00 and the loop
# index at +0x68. That index is the ordinal, it is never dropped, and it is
# readable off an object the colour wrapper is already holding -- which is what
# retired the inline constructor hook this used to need.
vtable IND_DECK_ID          class=N17indicator_control23IndicatorWithIdentifierINS_17DeckIndicatorKindEEE

# --- mods/xpad/deck.c: the three panel controls the X-PAD borrows while it is
# open. All three arrive as an AsyncTask whose run slot IS the gesture, so
# swallowing that run is the whole of the gate.
#
# MEMORY and CALL/DELETE capture nothing but `this` -- the TriggerType is
# consumed before the task is made, so each run is one press. VINYL captures its
# float at +0x18 and the VinylSpeedAdjustKind at +0x1c (sub_1055850:
# `ldr s0, [x0,#0x18]` / `ldr w1, [x0,#0x1c]`).
vtable MEMCUE_MEMORY_TASK class=*N4meow16AsyncTaskBoxBase9AsyncTaskIZN13input_devices16MemoryCueHandler9memoryCueENS2_11TriggerTypeERKNS2_23DeckInputDevicePriorityEEUlvE_EE
vtable MEMCUE_DELETE_TASK class=*N4meow16AsyncTaskBoxBase9AsyncTaskIZN13input_devices16MemoryCueHandler9deleteCueENS2_11TriggerTypeERKNS2_23DeckInputDevicePriorityEEUlvE_EE
vtable VINYL_ADJ_TASK     class=*N4meow16AsyncTaskBoxBase9AsyncTaskIKZN9dj_player22PlayPauseControlFacade16vinylSpeedAdjustEfNS2_20VinylSpeedAdjustKindEEUlvE_EE

# --- mods/xpad/deck.c: the deck's own QUANTIZE, which the sampler follows.
#
# gui::QuantizeState listens to two settings and keeps both in itself:
#   +0x8c  the MODE, a bool   (sub_17b7f90: *(state+0x8c) = *(task+0x20) == 1)
#   +0x90  the BEAT VALUE     (sub_17b7f58: *(state+0x90) = *(task+0x18), 1..4)
#
# The two closures hold the state at DIFFERENT offsets -- the mode task keeps it
# at +0x18 and the value task at +0x20 -- so each hook reads its own, and both
# then read the pair off the object rather than off the closure.
vtable QUANT_MODE_TASK  class=*N4meow16AsyncTaskBoxBase9AsyncTaskIZN3gui13QuantizeState16onSettingChangedERKN4juce10IdentifierERKN19application_setting14Value_QuantizeEEUlvE_EE
vtable QUANT_VALUE_TASK class=*N4meow16AsyncTaskBoxBase9AsyncTaskIZN3gui13QuantizeState16onSettingChangedERKN4juce10IdentifierERKN19application_setting19Value_QuantizeValueEEUlvE_EE

# =============================================================================
# virtual functions -- resolved by reading the slot, no address stored
# =============================================================================

slot LABEL_PAINT            vtable=LABEL              off=0xd0
slot BTN_MOUSEDOWN          vtable=BTN                off=0x28
slot TOUCHARIA_MOUSEDOWN    vtable=TOUCHARIA          off=0x28
slot TOUCHARIA_PAINT        vtable=TOUCHARIA          off=0xd0
slot DISPLAY_REFRESH_TIMERCB vtable=DISPLAY_REFRESH   off=0x10

slot BROWSE_TITLE_PAINT     vtable=BROWSE_TITLE       off=0xd0
slot ROWCOMP_MOUSEDOWN      vtable=ROWCOMP            off=0x28
slot ROWCOMP_MOUSEDRAG      vtable=ROWCOMP            off=0x30
slot ROWCOMP_MOUSEUP        vtable=ROWCOMP            off=0x38
slot ROWCOMP_PAINT          vtable=ROWCOMP            off=0xd0

slot DJSET_NUMROWS          vtable=DJSET_MODEL        off=0x10
slot DJSET_PAINTCELL        vtable=DJSET_MODEL        off=0x20
slot DJSET_SELCHANGED       vtable=DJSET_MODEL        off=0x60
slot DJSET_RNUMROWS         vtable=DJSET_RMODEL       off=0x10
slot DJSET_RCELLCLICK       vtable=DJSET_RMODEL       off=0x30
slot DJSET_RREFRESH         vtable=DJSET_RMODEL       off=0x28
slot DJSET_RSELCHANGED      vtable=DJSET_RMODEL       off=0x60
slot VIEW_MOUSEDOWN         vtable=UTILITY_VIEW       off=0x28
slot VIEW_TOUCHSEL          vtable=UTILITY_VIEW       off=0x178
slot VIEW_INPUT             vtable=UTILITY_VIEW       off=0x190
slot VIEW_SWITCH            vtable=UTILITY_VIEW       off=0x198
slot SOURCE_SELECT_BUILD    vtable=SOURCE_SELECT_VIEW off=0x178
slot VIEW_ROWCHANGED        vtable=UTILITY_AS_MODEL_LISTENER off=0x0
slot VIEW_KBD_KEY           vtable=UTILITY_AS_KBD_LISTENER   off=0x0

slot GFX_SETFILL            vtable=GFX_RENDERER       off=0x90
slot GFX_DRAWIMAGE          vtable=GFX_RENDERER       off=0xc8
slot IMAGECOMP_PAINT        vtable=JUCE_IMAGECOMPONENT off=0xd0

slot PCM_THRU_READ          vtable=PCM_THRU           off=0x10
slot PCM_SEQ_READ           vtable=PCM_SEQ            off=0x10
slot PCM_SIMPLE_READ        vtable=PCM_SIMPLE         off=0x10
slot PCM_PAGEBUF_READ       vtable=PCM_PAGEBUF        off=0x10
slot PCM_STRETCH_READ       vtable=PCM_STRETCH        off=0x10
slot PCM_LOADRESULT_RUN     vtable=PCM_LOADRESULT_TASK off=0x10
slot PCM_PREVIEW_READ       vtable=PCM_PREVIEW        off=0x10
slot TSMGR_OPERATE          vtable=TSMGR              off=0x98
slot TSMGR_SETSOURCE        vtable=TSMGR              off=0x38

slot WAVE_DOREQUEST         vtable=WAVE_REQ_HANDLER   off=0x10
slot GRID_RECEPTION_REPLY   vtable=GRID_RECEPTION      off=0x18
slot WAVE_RECEPTION_REPLY   vtable=WAVE_RECEPTION      off=0x28
slot WAVE_RECEPTION_RGB_REPLY  vtable=WAVE_RECEPTION_RGB  off=0x30
slot WAVE_RECEPTION_BLUE_REPLY vtable=WAVE_RECEPTION_BLUE off=0x38
slot WAVE_ANALYZE           vtable=WAVE_ANALYZE_CTRL  off=0x10

# Not hooked and not called: this is the ANCHOR the link() below is found from.
# doRequest is the deck's own registerBeatGridOffset call site, and its first BL
# is the one that resolves the cache.
slot BEATGRID_OFF_REGIST_REQ vtable=BEATGRID_OFF_REGIST off=0x10 ren=0x88e7a0
slot WAVEFORM_THROW          vtable=WAVEFORM_REQ_THROWER off=0x10 ren=0xcc5e50

slot GATE_PRESSPAD_RUN      vtable=GATE_PRESSPAD_TASK  off=0x10
slot GATE_RELEASEPAD_RUN    vtable=GATE_RELEASEPAD_TASK off=0x10
slot GATE_PLAYPAUSE_RUN     vtable=GATE_PLAYPAUSE_TASK off=0x10
slot GATE_BACK_CUE          vtable=GATE_RELEASEOP_TASK off=0x10 ren=0x1a49ec0
slot GATE_PADREL_SENTINEL   vtable=GATE_HOTCUE_HANDLER off=0x20

slot MEMCUE_MEMORY_RUN      vtable=MEMCUE_MEMORY_TASK  off=0x10
slot MEMCUE_DELETE_RUN      vtable=MEMCUE_DELETE_TASK  off=0x10
slot VINYL_ADJ_RUN          vtable=VINYL_ADJ_TASK      off=0x10
slot QUANT_MODE_RUN         vtable=QUANT_MODE_TASK     off=0x10
slot QUANT_VALUE_RUN        vtable=QUANT_VALUE_TASK    off=0x10

# ICueLoopSetter, one hop out through CueController. CUEING is the CUE BUTTON's
# whole action -- set when parked off the cue, back-cue when playing -- so it is
# hooked to read the enum values the deck itself uses. SETPOINT is the
# unconditional "put this cue kind at the play position" smart cue wants.
slot CUE_CUEING             vtable=CUE_CONTROLLER      off=0x10
slot CUE_SETPOINT           vtable=CUE_CONTROLLER      off=0x18

# CueController's forwarder onto the facade's +0x78. The shim never calls it; it
# is here only as the BL anchor for CUE_LINK_FACADE. Every CueController entry
# point links the facade at +0x18 before using it, but GCC 5.2.1 INLINED that
# link into setPoint and cueing on the Renesas builds, so those two carry no BL
# to key on. This forwarder still calls it out of line on both compilers, and on
# the RK3399 it is the only BL in the whole function.
slot CUE_FACADE_FWD         vtable=CUE_CONTROLLER      off=0xa0    ren=0x848e20

slot PREVIEW_SET            vtable=PREVIEW_CTL         off=0x10
slot PREVIEW_CLEAR          vtable=PREVIEW_CTL         off=0x18

slot HUI_MC_UPDATE          vtable=HUI_MULTICOLOR      off=0x10

# =============================================================================
# free functions -- masked signatures
# =============================================================================

# --- juce, shared by mod_menu.c / ui.c / decode.c / wave_req_probe.c ---
func JUCE_STRING_CTOR_CSTR   addr=0x1a2fea0 ren=0x1c51670
func JUCE_STRING_DTOR        addr=0x19f87f0 ren=0x1c2ec20
func JUCE_STRING_CTOR_EMPTY  addr=0x1a0b700 ren=0x1c30af0
func JUCE_VAR_CTOR_STRING    addr=0x19fe140 ren=0x1c22b60
func JUCE_VAR_DTOR           addr=0x19f5a50
func JUCE_VALUE_SETVALUE     addr=0x1a835f0
func JUCE_FONT_DTOR          addr=0x1ad57b0 ren=0x1d006e0
func JUCE_GFX_SETFONT        addr=0x1aab750 ren=0x1cccc90
func JUCE_GFX_SETCOLOUR      addr=0x1aab300 ren=0x1cfb2c0
func JUCE_GFX_FILLRECT_INT   addr=0x1aab840 ren=0x1cccd70
func JUCE_GFX_DRAWTEXT       addr=0x1ae7510 ren=0x1d07cb0
func JUCE_COMP_REPAINT_RECT  addr=0x1b82530 ren=0x1d91490
func JUCE_COMP_REPAINT       addr=0x1b84d60 ren=0x1d93c80
func JUCE_COMP_SETCOLOUR     addr=0x1b3a420 ren=0x1d50d20
func JUCE_COMP_SETBOUNDS     addr=0x1b9b7d0 ren=0x1d98ba0
func JUCE_COMP_SETTOPLEFT    addr=0x1b9c470 ren=0x1d994b0
func JUCE_COMP_ADDVISIBLE    addr=0x1b99ea0 ren=0x1daf490
func JUCE_COMP_GRABFOCUS     addr=0x1b8a260 ren=0x1da5680

# juce::TableHeaderComponent, which gui::TrackListHeader is unmodified except for
# its mouseDown. The browse EDIT mode reads the sort off it, forces its own, and
# hands the DJ's back afterwards.
#   getSortColumnId()                -- the column whose flags carry 0x20|0x40
#   setSortColumnId(id, forwards)    -- clears those on every column, sets one,
#                                       repaints and triggers the async re-sort
func JUCE_HDR_GET_SORTCOL    addr=0x1b55fe0 ren=0x1d675c0
func JUCE_HDR_SET_SORTCOL    addr=0x1b88620 ren=0x1d96fe0
func JUCE_LABEL_CTOR         addr=0x1b93240 ren=0x1daced0
func JUCE_LABEL_SETFONT      addr=0x1b85c30 ren=0x1d94dd0
func JUCE_LABEL_JUSTIFY      addr=0x1b855a0 ren=0x1d947a0
func JUCE_LABEL_SETTEXT      addr=0x1bd0270 ren=0x1ddc380
func JUCE_IDENTIFIER_CTOR    addr=0x1a40c00 ren=0x1c6e500
func JUCE_STRARR_CTOR        addr=0x1a16e90 ren=0x1c3afb0
func JUCE_STRARR_ADD         addr=0x1a173e0 ren=0x1c6b2e0
func JUCE_STRARR_DTOR        addr=0x1a47ef0 ren=0x1c68600
func JUCE_LISTBOX_SELECTROW  addr=0x1bb8cf0 ren=0x1dd7940
func JUCE_LISTBOX_CURRENTROW addr=0x1b54da0 ren=0x1d663c0
func JUCE_LISTBOX_UPDATE     addr=0x1bb6320 ren=0x1dc4490
func JUCE_EDITOR_SETTEXT     addr=0x1bd70a0 ren=0x1de3bf0
func JUCE_EDITOR_CTOR        addr=0x1bdb390 ren=0x1de7a50
func JUCE_EDITOR_SETFONT     addr=0x1bac9d0 ren=0x1da2050
func JUCE_EDITOR_JUSTIFY     addr=0x1b56980

# --- EP122's own ---
func EP122_FONT_BUILD        addr=0x17386d8 ren=0x195a090
func CREATE_READER_FOR       addr=0xa36bd8 ren=0xad2230
func SET_QUICKMENU_MODE      addr=0x14807d0 ren=0x163a3d0
func KBD_SHOW                addr=0x1416d48 ren=0x15c48c0
func KBD_HIDE                addr=0x1416cf0 ren=0x15c4510
func KBD_KEY_TO_TEXT         addr=0x142acd0 ren=0x15de700
func ENTER_DJSETTING         addr=0x1412920 ren=0x15bd3b0
func POPUP_CTOR              addr=0x15547d0 ren=0x1722900
func POPUP_SETMESSAGE        addr=0x154f9d0 ren=0x171ea40
# v3.22 grew a second function that is byte-identical to this one for 41
# instructions and then differs only in a BL offset, which the signature masks
# -- so no length disambiguates them (checked to 64). Scope it instead: only
# the real one is called from SourceSelectView's builder, and the twin is not
# called from anywhere at all. nth=0 because the scope leaves exactly one.
func POPUP_ADDLISTENER       addr=0x154fa60 in=SOURCE_SELECT_BUILD span=0x400 nth=0 ren=0x171ec60
func RMODEL_SETSTRINGS       addr=0x1578bf8 ren=0x174458c
func SRC_CTOR                addr=0xa4df00 ren=0xaec510

# track_info_repository::AsyncCommand::RequestID::RequestID()
#
# Every request the repository takes carries one, and it issues itself: the ctor
# writes the type tag and then an atomic ++ of a process-wide counter. Sixteen
# bytes, the id an int32 at +8. Called rather than imitated so the counter stays
# the deck's.
#
# Its body is eight instructions of stock prologue, and every build carries a
# second RequestID ctor of the same shape: both store a type tag from an ADRP/ADD
# pair and bump a counter, and the mask removes exactly the two globals that tell
# them apart. Length cannot separate them -- the count stays flat for as long as
# the window is inside the function, and growing it past the end only reads the
# NEXT function, which is a different one per build. So both variants are scoped
# by the thrower, which calls the real ctor and never the twin on every extracted
# build; the twin is the one with five call sites where the real one has sixty-odd.
func ASYNC_REQUEST_ID        addr=0x11e0138 in=WAVEFORM_THROW span=0x400 nth=0 ren=0x136b1c0 ren_in=WAVEFORM_THROW ren_span=0x400 ren_nth=0

# djdb's UPDATE, the sibling of the insert at 0x1c66f80:
#
#   int (const char *table, const char *index, ?, ?, const char *op,
#        int nkeys, void **keyvals, int ncols, const int32 *colids,
#        void **colvals)
#
# Same readiness gate as the insert (NULL context -> -10001, state != 2 ->
# -10025), resolves the table and the index by NAME, and converts each value by
# the column's own declared type -- so a caller states a column by index and
# hands over a pointer, exactly as the deck's own DsqlTrackUpdater does for
# RATING. See mods/db/djdb.c.
func DJDB_UPDATE             addr=0x1c426a0
# music_library::ListCacheCollector::removePlaylistTrackListCache(unsigned).
# The deck browses out of a list cache, so a reorder that only reaches the
# database is a reorder nothing on screen ever shows. This is the deck's own way
# of saying "that playlist's rows are stale". CALLED, never hooked -- its first
# instruction pair holds an ADRP, which mod_patch_fn refuses to displace.
func ML_DROP_PLAYLIST_CACHE  addr=0xf15f70 ren=0x1062110
# ...and the COLLECTOR to call it on, which is a member of the InformationUpdater
# and reachable from nowhere we stand. This is the capacity trim at the top of
# ListCacheCollector::createListCache, so it takes the collector as its first
# argument and runs every time the deck caches a list -- which is every list the
# DJ opens for the first time. Hooked only to read that pointer.
func ML_CACHE_TRIM           addr=0xf16688 ren=0x1062730
# The other two the deck drops alongside the track list whenever a playlist's
# contents change: removePlaylistListCache(unsigned) and, with a 16-bit browse
# category, removeHierarchyCache. Called with the same arguments the deck's own
# delete paths use.
func ML_DROP_PLAYLIST_LIST   addr=0xf15f00 ren=0x1062070
func ML_DROP_HIERARCHY       addr=0xf16220 ren=0x10624a0

# meow::MappedObjPtr<usecase::deck::IUsecaseDeck>::link()
#
# The pad handler keeps one of these at +0x30 and it starts EMPTY: the cached
# pointer is filled from the object-id map on first use, which is why reading the
# slot raw found nothing until the deck's own press path had run once -- and why
# the first pad press after a track load fell through every behaviour that needs
# the CueController.
#
# Idempotent and cheap: a non-null cache returns 1 without touching the map.
#
# A `call` rather than a `func`, because its signature is worthless: link() is a
# template instantiation with a stock prologue and names hundreds of functions,
# more than either scan will hold. What IS exact is that the deck's own back-cue
# links this very member immediately before resolving it, so the BL that does it
# is the identity.
call CUE_LINK_DECK           addr=0x17fe0b0 from=GATE_BACK_CUE insn=29 ren=0x1a32630 ren_insn=43

# meow::MappedObjPtr<dj_player::ICueLoopSetter>::link()
#
# The SECOND one on the way to the cue table: the CueController keeps the facade
# at +0x18 and it starts empty too, so linking only the handler's still left the
# first press of a process with nothing to read. CueController::setPoint links it
# before every use -- but only on the RK3399, where it is a BL. CUE_FACADE_FWD
# above is the forwarder that still makes that call out of line on both SoCs.
call CUE_LINK_FACADE         addr=0x7fcfd8 from=CUE_FACADE_FWD insn=7 ren=0x84e6d0 ren_insn=13

# meow::MappedObjPtr<track_info_repository::ITrackInfoRepositoryCache>::link()
#
# The THIRD of these, and the one that reaches a named singleton rather than a
# member somebody else already filled in: the pair it takes is ours, so linking
# is how the shim asks the app's own object map for the app's own cache. Same
# `call` reasoning as the two above -- link() is a template body shared by
# hundreds of instantiations, and only the call SITE identifies which. The site
# is the deck's own registerBeatGridOffset, whose doRequest links the cache in
# its first BL.
call TIR_CACHE_LINK          addr=0x6ddba0 from=BEATGRID_OFF_REGIST_REQ insn=7 ren=0x70cfb0 ren_insn=11

# The two halves of "put a cue somewhere", both on the cue engine reached as
# *(ICueLoopSetter + 0x70) + 8:
#
#     setHere(engine, CueKind, quantize, desc)     at the PLAY HEAD
#     setAt  (engine, CueKind, PositionWithSourceInfo *, onGrid, desc)
#
# They are siblings over one setter (sub_10dd0d8), which is what actually fills
# a cue slot: position, source info, the refcounted source with its count taken,
# the exists byte at +0x20, the on-grid byte at +0x21, a version bump at +0x08,
# the state at +0x0c, and a reset of the second position block at +0x70. That
# list is the reason the slot is not written by hand.
#
# setHere is HOOKED and setAt is CALLED, which is the whole of PREVIEW HOTCUE:
# the deck's own press runs, with every guard and notification above it, and the
# one thing that changes is which position reaches the slot.
#
# `desc` is the cue's COLOUR -- three bytes sub_10de100 copies to slot+0x10, and
# each caller has its own. The hook forwards whatever its caller passed, which
# is why a preview assign comes out the colour the pad would have had.
func CUE_SET_HERE            addr=0x10ad670 ren=0x1227fb0
# gui::detailed_waveform renderBackground. Fills a 4-byte juce::Colour per column
# into the array renderWaveform_new then lerps the ink against, so this -- not the
# palette and not the provider -- is the colour the waveform's OUTER EDGE mixes with.
# Measured live: mode 1, and every entry 0x00000000.
func WAVE_RENDER_BG          addr=0x1624078 ren=0x1810af0
func CUE_SET_AT              addr=0x10ac768 ren=0x1226ab0

# EVERY LED THE APP LIGHTS goes through this one call:
#
#     ledWrite(holder, index, const uint8_t rgb[4])
#
# Exactly two callers, and they are the only two ways an indicator has of
# saying what colour it is -- hui::HuiIndicatorAbs::SingleColor's update
# (sub_1803a98) and MultiColor's (sub_1803ae0, vtable 0x21e7010 slot 2). The
# multi-colour one gets its answer from *(this+8) vtable +0x18, which returns a
# packed u64: byte 0 the LED index, bytes 4-6 the R, G and B.
#
# It is not a frame write. It records (index, colour) against the holder and
# dispatches to listeners, which is what makes it safe to call with a colour of
# our own: every listener downstream is told the new one, and nothing is left
# holding the old.
#
# CALLED, not hooked -- there is no trampoline in this tree and a free function
# has no slot to repoint. The interception is HUI_MC_UPDATE below, which is a
# vtable slot; a wrapper there decides the colour and calls this with it.
#
# The app, not the ioctl the frame eventually leaves through: mods/ has to stand
# on EP122 alone, it is being separated from the emulation shim, and a syscall
# hook would not survive that.
func HUI_LED_WRITE           addr=0x18f2c40 ren=0x1b19650



# --- mods/db/db.c ---
# music_library::SqliteUpdateTransaction::exec, from the assert path
# "Source/Domain/MusicLibrary/Server/DataBase/SQLite/AccessWrapper/
# SqliteUpdateTransaction.h". Its first argument is the transaction object and
# *arg is the live sqlite3*, which is the ONLY reason this is hooked: the
# handle is already open and already keyed, so a mod never sees the SQLCipher
# key and never opens a database of its own. Every library write the deck makes
# passes through here, which includes the history row written on a track load,
# so a handle arrives without the DJ doing anything unusual.
# 8 instructions is unique in this build but matches twice in v3.13, so the
# recorded count no longer holds and the symbol is refused. 12 is unique in
# every RK3399 build from v3.13 to v3.22.
func SQLITE_UPDATE_EXEC      addr=0xe27060 min=12 ren=0xfaf100

# --- mods/db/djdb.c ---
# The djdb context accessor. Rekordbox media does NOT go through SQL at all --
# Dsql*/DeviceSQL calls this library by table name -- so this is the only route
# to a stick, and it is an accessor rather than a file: a global pointer with an
# override hook, which is the deck's own instantiated mechanism and the only
# thing we are willing to reach a library through.
func DJDB_CONTEXT            addr=0x1c72a60

# djdb's row insert -- sub_1c66f80(const char *table, int ncols, void **values).
# Hooked to OBSERVE, because the context accessor above answers NULL from an
# idle thread: the context is live inside an operation and not between them, so
# the one moment a mod can look at the media library is while the deck is
# already using it. Same shape as the SQL side's handle capture.
func DJDB_ROW_INSERT         addr=0x1c41e90

# The other row operation (seven arguments, same context preamble) and the two
# transaction openers. Hooked for the same reason and in the same way: any one
# of them firing means a live context, and which one the deck reaches for
# depends on what it is writing -- a hot cue does not take the insert path.
# NOT an update -- the QUERY. sub_1c67420(table, index, rowFn, rowArg, op,
# nkeys, keyvals): it finds the rows an index key matches AND for which rowFn
# returns true. Settled from DsqlUpdateTransaction slots 10/11, which pass
# ("djdbSongHistory", NULL, sub_e696d0, &id, NULL, 0, 0) -- a NULL index meaning
# all rows, and a filter that compares one column. It was named ROW_UPDATE while
# its argument roles were unknown; the old name was a guess.
func DJDB_QUERY              addr=0x1c42330
func DJDB_TXN_A              addr=0x1c44030
func DJDB_TXN_B              addr=0x1c440e0

# djdb's page store: every row on the media travels through this pair.
#
#   read (self, buf, index, &eof)   write(self, buf, index)
#
# Both take the file offset as `index * *(self + 0x10)`, seek the handle at
# *(self + 0x00) and move exactly a page. The write treats index 0 as the
# header, taking its length from *(self + 0x18) and its handle from
# *(self + 0x08) when those are set.
#
# Neither has a direct caller: they are entries 1 and 2 of a six-function
# storage backend built at 0x1c79300. The write is also the ONE moment a live
# djdb context is guaranteed -- the deck buffers a whole session in memory and
# flushes here at EJECT -- so it is where the table registry gets read.
func DJDB_PAGE_READ          addr=0x1c54180
func DJDB_PAGE_WRITE         addr=0x1c54020

# The page manager above the adaptor -- "PGM:PGM", registered by DBService.cpp
# with PAGESZ=4096 and NBUFS=6144, i.e. a 24 MiB pool of page buffers holding
# the media library in memory.
#
# PageMarkDirty is what decides whether a changed page is ever written: it sets
# the descriptor's flag 2, puts the page index on the manager's modified list,
# and stamps the transaction into the page. Both variants name themselves in
# their own assertion string ("PageMarkDirty: buffer is invalid!"); the first
# also has a write-through branch, taken when *(int16 *)(pgm + 0x20) is set.
# sub_1c5c1b0(row, col) -- reads one column of a row and returns a pointer to
# the value. A three-instruction adapter: owner = *row, then tail-calls
# owner->vtable[0x80](owner, *(*(row+0x18)+8), col, 0). This is the primitive a
# row filter is built from, and the read side of a playlist walk.
func DJDB_ROW_COL            addr=0x1c370c0

func DJDB_MARK_DIRTY         addr=0x1c3ea30
func DJDB_MARK_DIRTY_PLAIN   addr=0x1c3d4c0

# TE_DPgm_BufferGetPage(handle, index, &desc) -- names itself in its own error
# string. Fetches a page into the pool, from the hash table if it is resident
# and through the adaptor if not, and hands back the DESCRIPTOR: flag byte at
# +0x00, the page's raw bytes at +0x08.
#
# It is hooked for its FIRST ARGUMENT. Every page the deck reads comes through
# here, so this is the one place a live page-manager handle can be had -- and
# both BufferGetPage and PageMarkDirty need one.
func DJDB_BUFFER_GET_PAGE    addr=0x1c3f6f0

# THE LIBRARY SERVER'S OWN THREAD, reached by being on it rather than by posting
# to it. A djdb context is a per-thread DB handle, and no djdb entry point fires
# during browsing -- so a held write needs a moment on that thread that is not a
# djdb operation.
#
# client_server_framework::ServerThreadBase::receiveMessage posts its body as an
# AsyncTask, so this box's run slot IS that thread popping one message and
# dispatching it: lock, unhook the queue head, unlock, then
# (*(*srv + 0x50))(srv, msg, kind). Draining AFTER it is the cleanest moment
# there is -- right thread, mutex released, nothing in flight.
#
# A SLOT, deliberately. The last attempt at this used a masked signature on a
# template instantiation and went unresolved on the deck, which disables every
# mod. An RTTI class name is exact and a slot read cannot be ambiguous.
#
# The vtable is shared by every ServerThreadBase, so this fires on the SD, cloud
# and repository threads too. That costs one context check: only a thread with a
# DB handle has a context, so the drain selects itself.
vtable SRV_MSG_TASK   class=*N4meow16AsyncTaskBoxBase9AsyncTaskIZN23client_server_framework16ServerThreadBase14receiveMessageEPN11data_stream17DataStreamMessageEjEUlvE_EE
slot   SRV_MSG_RUN    vtable=SRV_MSG_TASK off=0x10
# dj_player's state notification server watches the player and reports per state:
# one check* task each, every one an AsyncTaskBox task like the one above.
# checkTrackEnd(PlayerState const&) is where "the track reached its natural end"
# is decided -- and, since no vtable in this build names ITrackEndNotificationLisner,
# the listener the server offers for exactly that, where whatever the deck does
# about it is decided too. That is the place to look for the one behaviour worth
# knowing here: the advance to the next streaming track happens on the performance
# view and not on the browse view, so the difference is in this task, not in a
# listener.
#
# A SLOT, for the same reason as SRV_MSG_RUN: the task type is named by RTTI, so
# the box's run slot is exact, where a masked signature on a template
# instantiation is not.
vtable TRACKEND_TASK  class=*N4meow16AsyncTaskBoxBase9AsyncTaskIZN9dj_player23StateNotificationServer13checkTrackEndERKNS2_11PlayerStateEEUlvE_EE
slot   TRACKEND_RUN   vtable=TRACKEND_TASK off=0x10
# The other half of the same pair: checkTrackChange is where "the loaded track is
# now a different one" is decided, which is the direct measurement of whether the
# end-of-track advance happened at all rather than of whether it was considered.
vtable TRACKCHANGE_TASK  class=*N4meow16AsyncTaskBoxBase9AsyncTaskIZN9dj_player23StateNotificationServer16checkTrackChangeERKNS2_11PlayerStateEEUlvE_EE
slot   TRACKCHANGE_RUN   vtable=TRACKCHANGE_TASK off=0x10
# The deck's own play/pause entry points, as tasks. Hooked as a CONTROL on the
# state checks above: unlike them this one certainly runs, so it says whether an
# AsyncTaskBox task is dispatched through its vtable slot at all in this build --
# and a play marker is worth having either way.
vtable PLAY_TASK  class=*N4meow16AsyncTaskBoxBase9AsyncTaskIKZN9dj_player22PlayPauseControlFacade4playENS2_17PlayStartBehaviorENS2_12SlipBehaviorEEUlvE_EE
slot   PLAY_RUN   vtable=PLAY_TASK off=0x10

# The deck's own play/pause entry points, as tasks. Hooked as a CONTROL on the
# state checks above: unlike them this one certainly runs, so it says whether an
# AsyncTaskBox task is dispatched through its vtable slot at all in this build --
# and a play marker is worth having either way.
vtable PAUSE_TASK  class=*N4meow16AsyncTaskBoxBase9AsyncTaskIKZN9dj_player22PlayPauseControlFacade5pauseENS2_13PauseBehaviorENS2_12SlipBehaviorEEUlvE_EE
slot   PAUSE_RUN   vtable=PAUSE_TASK off=0x10
# The state checks' BODIES, reached the way the deck actually reaches them.
#
# Hooked as functions rather than as the task types' run slots, which is a
# measurement and not a preference: the run slot of each such task vtable holds a
# four-instruction thunk (0x1117b00 for checkTrackEnd), and four slot patches --
# the two checks plus the play/pause facades, as a control -- were installed at
# boot and never fired once, through two full play-throughs, the second of which
# ended and advanced to the next track by itself.
#
# The body is what they do reach: checkTrackEnd's own state loop at 0x1117b18
# calls TRACKEND_INVOKE by direct BL, carrying the callable and a pointer to the
# state. (A call through a slot is an indirect branch, so the absence of a B or BL
# naming the thunk would prove nothing on its own -- the slot patches not firing is
# the measurement, and this call site is why.)
#
# THE INVOKER IS THE HOOK: it runs whenever the check's body runs, however it was
# reached, and it takes the state as its second argument -- the PlayerState the
# check is about to judge.
func TRACKEND_INVOKE    addr=0x111fd60
func TRACKEND_CHECK     addr=0x1117b18


# =============================================================================
# globals
# =============================================================================

# The skin's foreground/background juce::Colour pair. Both are reached by an
# ADRP/ADD pair from dozens of call sites; anchoring on one we already resolve
# means the address comes from the same instruction the deck itself executes.
# djdb's context slot: {byte override-flag, pad, void *ctx at +0x08,
# fn at +0x10, arg at +0x18}. The accessor answers *+0x08 when the flag is
# clear and calls the function when it is set -- read only, to tell "there is no
# context" apart from "the context is being scoped away from us".
data DJDB_CONTEXT_SLOT addr=0x6699290 from=DJDB_CONTEXT insn=0

data SKIN_FG  addr=0x6698798 from=KBD_SHOW   insn=10 ren=0x6748a98 ren_insn=14
data SKIN_BG  addr=0x66987f8 from=POPUP_CTOR insn=9 ren=0x6748b30 ren_insn=1

# =============================================================================
# deliberately not static
# =============================================================================

# The AudioReaderFactory singleton. Nothing in .text takes its address -- every
# caller of createReaderFor already holds it in a register -- and it lives in
# .bss, so there is nothing in the file either. It is found at RUN TIME by its
# vptr instead (ep122_find_instance), which needs only the class above.
capture READER_FACTORY   # -> ep122_find_instance(EP122_AUDIO_READER_FACTORY)

# The skin's white and grey text colours are NOT here and cannot be: an
# exhaustive scan for ADRP paired with add/ldr/str at every width finds zero
# references to the grey anywhere in .text, and neither is a class for RTTI to
# name. The MOD SETTINGS rows take both from the theme roles instead
# (mod_ui()->text / text_dim), which is where the rest of the mod UI gets its
# colours; no address is needed.

# --- mods/grid/panel.c: the BPM group in the grid-edit panel ---
# PRIMARY vtables only, used as TYPEINFO identities for juce_comp_find_class --
# these are model-first widgets and juce::Component is a VIRTUAL base of them, so
# `base=N4juce9ComponentE` cannot name the Component vtable (the generator says
# "0 vtables at offset-to-top 24", and it is right). Every vtable in a class's
# group carries the same typeinfo, so matching on that finds the subobject
# whichever vtable a pointer happens to point at. See the play-screen rack.
vtable GRIDPANEL          class=N3gui17detailed_waveform10GridAdjustE
# GridAdjustButton itself is ambiguous -- it is the base of the three below and
# resolves to four address points -- so the concrete classes are named instead.
vtable GRIDBTN_SNAP       class=N3gui18grid_adjust_button14SnapGridButtonE
vtable GRIDBTN_SHIFT      class=N3gui18grid_adjust_button15ShiftGridButtonE
vtable GRIDBTN_RESET      class=N3gui18grid_adjust_button11ResetButtonE

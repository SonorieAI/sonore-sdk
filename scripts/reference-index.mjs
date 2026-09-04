// SPDX-License-Identifier: Apache-2.0
// the reference's own class list, so "no gaps" can mean something.
//
// ── Why this file exists ────────────────────────────────────────────────────
//
// The feature map is checked in two directions already. feature-map.mjs proves
// every capability it CLAIMS is still in the headers, and that every capability
// it says is deliberately absent really is. unclaimed.mjs proves the reverse:
// every public type in the headers appears in the map.
//
// Both of those are closed loops between the map and our own code. Neither can
// see the thing that actually matters, which is whether the map has a row for
// something the reference has and we have never thought about. "0 gaps" has therefore
// only ever meant "0 among the rows somebody wrote down" -- a caveat repeated
// in commit after commit and never mechanised.
//
// It was not hypothetical. Walking six gui_basics groups against the map
// found THIRTY-SIX classes with no row at all: ResizableBorderComponent,
// ResizableEdgeComponent, AnimatedPosition, Toolbar, SidePanel, MenuBarComponent,
// the whole PropertyComponent family, and more. The map reported one real gap at
// the time. It could not have reported these, because a class nobody has written
// down cannot be counted as missing.
//
// The graphics groups went better -- most of those names were already
// claimed -- and the failures there were a different and quieter kind. JPEG's
// absence had a real, argued reason sitting in png.h since the day it was
// written, and the audit counted it as undecided because the reason was in the
// header and not where any count could see it. A decision nobody can find is
// worth what an undecided one is.
//
// ── What is in here, and what is not ────────────────────────────────────────
//
// Class NAMES, read off the reference's public documentation index. Names are the
// factual surface of an API -- what the classes are called -- and nothing here
// is copied from the reference: no source, no documentation prose, no implementation,
// no signatures. That is the same basis the dsp audit was done on. The
// rule against copying the reference source is not weakened by knowing what the reference's
// classes are called, and this file is careful to be only that.
//
// ── Partial, and it says which parts ────────────────────────────────────────
//
// AUDITED lists the groups walked so far. PENDING lists the ones that have
// not been, by name, so this file can never be mistaken for a complete
// checklist. A partial list that states its own edges is worth a great deal
// more than none; a partial list that implies completeness is worth less than
// none.
export const AUDITED = {
  'gui_basics-widgets': [
    'ComboBox', 'ImageComponent', 'Label', 'ListBox', 'ListBoxModel', 'ProgressBar',
    'Slider', 'SliderListener', 'TableHeaderComponent', 'TableListBox', 'TableListBoxModel',
    'TextEditor', 'Toolbar', 'ToolbarItemComponent', 'ToolbarItemFactory',
    'ToolbarItemPalette', 'TreeView', 'TreeViewItem',
  ],
  'gui_basics-layout': [
    'AnimatedPosition', 'AnimatedPositionBehaviours', 'BorderedComponentBoundsConstrainer',
    'ComponentAnimator', 'ComponentBoundsConstrainer', 'ComponentBuilder',
    'ComponentMovementWatcher', 'ConcertinaPanel', 'FlexBox', 'FlexItem', 'Grid', 'GridItem',
    'GroupComponent', 'MultiDocumentPanel', 'MultiDocumentPanelWindow',
    'ResizableBorderComponent', 'ResizableCornerComponent', 'ResizableEdgeComponent',
    'ScrollBar', 'SidePanel', 'StretchableLayoutManager', 'StretchableLayoutResizerBar',
    'StretchableObjectResizer', 'TabBarButton', 'TabbedButtonBar', 'TabbedComponent',
    'Viewport',
  ],
  'gui_basics-windows': [
    'AlertWindow', 'CallOutBox', 'ComponentPeer', 'DialogWindow', 'DocumentWindow',
    'MessageBoxOptions', 'NativeMessageBox', 'NativeScaleFactorNotifier', 'ResizableWindow',
    'ScopedMessageBox', 'ThreadWithProgressWindow', 'TooltipWindow', 'TopLevelWindow',
    'VBlankAttachment', 'WindowUtils',
  ],
  'gui_basics-menus': [
    'BurgerMenuComponent', 'MenuBarComponent', 'MenuBarModel', 'PopupMenu',
  ],
  'gui_basics-properties': [
    'BooleanPropertyComponent', 'ButtonPropertyComponent', 'ChoicePropertyComponent',
    'MultiChoicePropertyComponent', 'PropertyComponent', 'PropertyPanel',
    'SliderPropertyComponent', 'TextPropertyComponent',
  ],
  'graphics-images': [
    'GIFImageFormat', 'Image', 'ImageCache', 'ImageConvolutionKernel', 'ImageFileFormat',
    'ImagePixelData', 'ImageType', 'JPEGImageFormat', 'NativeImageType', 'PNGImageFormat',
    'ScaledImage', 'SoftwareImageType',
  ],
  'graphics-geometry': [
    'AffineTransform', 'BorderSize', 'EdgeTable', 'Line', 'Parallelogram', 'Path',
    'PathFlatteningIterator', 'PathStrokeType', 'Point', 'Rectangle', 'RectangleList',
  ],
  'graphics-colour': [
    'Colour', 'ColourGradient', 'FillType', 'PixelARGB', 'PixelAlpha', 'PixelRGB',
  ],
  'graphics-fonts': [
    'AttributedString', 'Font', 'FontOptions', 'GlyphArrangement', 'PositionedGlyph',
    'TextLayout', 'Typeface', 'TypefaceMetrics',
  ],
  'graphics-drawables': [
    'Drawable', 'DrawableComposite', 'DrawableImage', 'DrawablePath', 'DrawableRectangle',
    'DrawableShape', 'DrawableText', 'StrokeOptions',
  ],
  'graphics-justification': ['Justification', 'RectanglePlacement'],
  'graphics-fontfeatures': [
    'ColourLayer', 'FontFeatureSetting', 'FontFeatureTag', 'FontVariableSetting',
    'GlyphArrangementOptions', 'GlyphLayer', 'ImageLayer',
  ],
  'graphics-effects': [
    'DropShadow', 'DropShadowEffect', 'GlowEffect', 'ImageEffectFilter',
  ],
  'product_unlocking': [
    'OnlineUnlockStatus', 'OnlineUnlockForm', 'KeyGeneration', 'KeyFileUtils',
    'TracktionMarketplaceStatus',
  ],
  'core': [
    // containers and strings
    'AbstractFifo', 'Array', 'DynamicObject', 'HashMap', 'ListenerList', 'NamedValueSet',
    'Optional', 'OwnedArray', 'PropertySet', 'ReferenceCountedArray', 'ScopedValueSetter',
    'SortedSet', 'Span', 'SparseSet', 'var', 'String', 'StringArray', 'StringPairArray',
    'StringPool', 'StringRef', 'Identifier', 'TextDiff', 'Base64', 'CharacterFunctions',
    'LocalisedStrings', 'NewLine',
    // files and streams
    'File', 'FileFilter', 'FileInputStream', 'FileOutputStream', 'FileSearchPath',
    'MemoryMappedFile', 'RangedDirectoryIterator', 'TemporaryFile', 'WildcardFileFilter',
    'DirectoryEntry', 'InputStream', 'OutputStream', 'MemoryInputStream',
    'MemoryOutputStream', 'BufferedInputStream', 'SubregionStream', 'InputSource',
    'FileInputSource',
    // json and xml
    'JSON', 'JSONUtils', 'VariantConverter', 'XmlAttribute', 'XmlDocument', 'XmlElement',
    // maths
    'BigInteger', 'Expression', 'MathConstants', 'NormalisableRange', 'Random', 'Range',
    'StatisticsAccumulator', 'Tolerance',
    // memory
    'Atomic', 'ByteOrder', 'HeapBlock', 'LeakedObjectDetector', 'MemoryBlock',
    'ReferenceCountedObject', 'ReferenceCountedObjectPtr', 'SharedResourcePointer',
    'SingletonHolder', 'WeakReference', 'OptionalScopedPointer',
    // threads
    'ChildProcess', 'CriticalSection', 'DynamicLibrary', 'HighResolutionTimer',
    'InterProcessLock', 'Process', 'ReadWriteLock', 'SpinLock', 'Thread', 'ThreadLocalValue',
    'ThreadPool', 'ThreadPoolJob', 'TimeSliceClient', 'TimeSliceThread', 'WaitableEvent',
    'GenericScopedLock',
    // time, logging, system
    'PerformanceCounter', 'RelativeTime', 'ScopedTimeMeasurement', 'Time', 'SystemStats',
    'Logger', 'FileLogger', 'Uuid', 'Result', 'ScopeGuard', 'ConsoleApplication',
    'ArgumentList', 'RuntimePermissions', 'WindowsRegistry',
    // network
    'DatagramSocket', 'IPAddress', 'MACAddress', 'NamedPipe', 'StreamingSocket', 'URL',
    'WebInputStream',
    // compression and testing
    'GZIPCompressorOutputStream', 'GZIPDecompressorInputStream', 'ZipFile', 'UnitTest',
    'UnitTestRunner',
    // android
    'AndroidDocument', 'AndroidDocumentInfo', 'AndroidDocumentIterator',
  ],
  'graphics-contexts': [
    'Graphics', 'LowLevelGraphicsContext', 'LowLevelGraphicsSoftwareRenderer',
    'ScopedBlendContext', 'ScopedBlendContextOptions',
  ],
  'audio_utils': [
    'AudioAppComponent', 'AudioDeviceSelectorComponent', 'AudioThumbnail',
    'AudioThumbnailBase', 'AudioThumbnailCache', 'AudioVisualiserComponent',
    'BluetoothMidiDevicePairingDialogue', 'KeyboardComponentBase', 'MPEKeyboardComponent',
    'MidiKeyboardComponent', 'AudioProcessorPlayer', 'SoundPlayer', 'AudioCDBurner',
    'AudioCDReader', 'Box2DRenderer',
  ],
  'audio_plugin_client': ['StandaloneFilterWindow', 'StandalonePluginHolder'],
  'opengl': [
    'Draggable3DOrientation', 'Matrix3D', 'Quaternion', 'Vector3D', 'OpenGLContext',
    'OpenGLFrameBuffer', 'OpenGLGraphicsContextCustomShader', 'OpenGLHelpers',
    'OpenGLImageType', 'OpenGLPixelFormat', 'OpenGLRenderer', 'OpenGLShaderProgram',
    'OpenGLTexture', 'OpenGLVersion', 'OpenGLAppComponent',
  ],
  'animation': [
    'Animator', 'AnimatorSetBuilder', 'AnimatorUpdater', 'Easings', 'SpringEasingOptions',
    'StaticAnimationLimits', 'VBlankAnimatorUpdater', 'ValueAnimatorBuilder',
  ],
  'cryptography': ['BlowFish', 'Primes', 'RSAKey', 'MD5', 'SHA256', 'Whirlpool'],
  'osc': [
    'OSCAddress', 'OSCAddressPattern', 'OSCArgument', 'OSCBundle', 'OSCColour',
    'OSCException', 'OSCMessage', 'OSCReceiver', 'OSCSender', 'OSCTimeTag', 'OSCTypes',
  ],
  // The top-level concepts only. midi_ci also has some sixty Message::*
  // structs, one per wire message of the protocol; they are in
  // NOT_CAPABILITIES as a family, with the reason there.
  'midi_ci': [
    'Device', 'DeviceFeatures', 'DeviceOptions', 'DeviceListener', 'Parser', 'MUID',
    'ChannelAddress', 'FunctionBlock', 'Encodings', 'Subscription', 'SubscriptionManager',
    'ProfileHost', 'ProfileDelegate', 'ProfileAtAddress', 'PropertyHost', 'PropertyDelegate',
    'PropertyExchangeResult', 'ResponderDelegate', 'Pagination',
  ],
  'dsp': [
    'AudioBlock', 'SIMDRegister', 'FilterDesign', 'Convolution', 'ConvolutionMessageQueue',
    'FFT', 'WindowingFunction', 'FastMathApproximations', 'LogRampedValue', 'LookupTable',
    'LookupTableTransform', 'Matrix', 'Phase', 'Polynomial', 'SpecialFunctions',
    'BallisticsFilter', 'DelayLine', 'DryWetMixer', 'FirstOrderTPTFilter',
    'LinkwitzRileyFilter', 'Oversampling', 'Panner', 'ProcessContextNonReplacing',
    'ProcessContextReplacing', 'ProcessSpec', 'ProcessorBase', 'ProcessorChain',
    'ProcessorDuplicator', 'ProcessorState', 'ProcessorWrapper', 'StateVariableTPTFilter',
    'Bias', 'Chorus', 'Compressor', 'Gain', 'LadderFilter', 'Limiter', 'NoiseGate',
    'Oscillator', 'Phaser', 'Reverb', 'WaveShaper',
  ],
  'audio_formats': [
    'AiffAudioFormat', 'CoreAudioFormat', 'FlacAudioFormat', 'LAMEEncoderAudioFormat',
    'MP3AudioFormat', 'OggVorbisAudioFormat', 'WavAudioFormat', 'WindowsMediaAudioFormat',
    'ARAAudioSourceReader', 'ARAPlaybackRegionReader', 'AudioFormat', 'AudioFormatManager',
    'AudioFormatReader', 'AudioFormatReaderSource', 'AudioFormatWriter',
    'AudioFormatWriterOptions', 'AudioSubsectionReader', 'BufferingAudioReader',
    'MemoryMappedAudioFormatReader', 'SamplerSound', 'SamplerVoice',
  ],
  'audio_devices': [
    'AudioDeviceManager', 'AudioIODevice', 'AudioIODeviceCallback', 'AudioIODeviceType',
    'SystemAudioVolume', 'MidiDeviceInfo', 'MidiDeviceListConnection', 'MidiInput',
    'MidiInputCallback', 'MidiMessageCollector', 'MidiOutput', 'AudioSourcePlayer',
    'AudioTransportSource',
  ],
  'audio_basics': [
    'AudioPlayHead', 'AudioBuffer', 'AudioChannelSet', 'AudioData',
    'AudioProcessLoadMeasurer', 'FloatVectorOperations', 'ScopedNoDenormals', 'MidiBuffer',
    'MidiFile', 'MidiKeyboardState', 'MidiMessage', 'MidiMessageSequence', 'MidiRPNDetector',
    'MidiRPNGenerator', 'MPEChannelAssigner', 'MPEChannelRemapper', 'MPEInstrument',
    'MPEMessages', 'MPENote', 'MPESynthesiser', 'MPESynthesiserVoice', 'MPEValue', 'MPEZone',
    'MPEZoneLayout', 'AudioSource', 'BufferingAudioSource', 'ChannelRemappingAudioSource',
    'IIRFilterAudioSource', 'MemoryAudioSource', 'MixerAudioSource', 'PositionableAudioSource',
    'ResamplingAudioSource', 'ReverbAudioSource', 'ToneGeneratorAudioSource', 'Synthesiser',
    'SynthesiserSound', 'SynthesiserVoice', 'ADSR', 'AudioWorkgroup', 'Decibels',
    'GenericInterpolator', 'IIRCoefficients', 'IIRFilter', 'Interpolators', 'Reverb',
    'SmoothedValue', 'WorkgroupToken',
  ],
  'events': [
    'ActionBroadcaster', 'ActionListener', 'AsyncUpdater', 'ChangeBroadcaster',
    'ChangeListener', 'LockingAsyncUpdater', 'ChildProcessCoordinator', 'ChildProcessManager',
    'ChildProcessWorker', 'InterprocessConnection', 'InterprocessConnectionServer',
    'NetworkServiceDiscovery', 'CallbackMessage', 'DeletedAtShutdown', 'ApplicationBase',
    'Message', 'MessageListener', 'MessageManager', 'MessageManagerLock',
    'MountedVolumeListChangeDetector', 'ScopedLibraryInitialiser', 'MultiTimer',
    'TimedCallback', 'Timer',
  ],
  'data_structures': [
    'ApplicationProperties', 'PropertiesFile', 'UndoManager', 'UndoableAction', 'CachedValue',
    'Value', 'ValueTree', 'ValueTreePropertyWithDefault', 'ValueTreeSynchroniser',
  ],
  'gui_extra': [
    'CPlusPlusCodeTokeniser', 'CodeDocument', 'CodeEditorComponent', 'CodeTokeniser',
    'CppTokeniserFunctions', 'LuaTokeniser', 'XmlTokeniser', 'FileBasedDocument',
    'ActiveXControlComponent', 'AndroidViewComponent', 'HWNDComponent', 'NSViewComponent',
    'UIViewComponent', 'XEmbedComponent', 'XEmbedComponentOptions', 'AnimatedAppComponent',
    'AppleRemoteDevice', 'BubbleMessageComponent', 'ColourSelector',
    'KeyMappingEditorComponent', 'PreferencesPanel', 'PushNotifications',
    'RecentlyOpenedFilesList', 'SplashScreen', 'SystemTrayIconComponent',
    'WebBrowserComponent', 'WebComboBoxRelay', 'WebControlParameterIndexReceiver',
    'WebSliderRelay', 'WebToggleButtonRelay', 'WebViewLifetimeListener',
  ],
  'gui_basics-accessibility': [
    'AccessibilityHandler', 'AccessibleState', 'AccessibilityActions',
    'AccessibilityCellInterface', 'AccessibilityNumericValueInterface',
    'AccessibilityRangedNumericValueInterface', 'AccessibilityTableInterface',
    'AccessibilityTextInterface', 'AccessibilityTextValueInterface',
    'AccessibilityValueInterface',
  ],
  'gui_basics-application': ['Application'],
  'gui_basics-commands': [
    'ApplicationCommandInfo', 'ApplicationCommandManager', 'ApplicationCommandManagerListener',
    'ApplicationCommandTarget', 'KeyPressMappingSet',
  ],
  'gui_basics-components': [
    'CachedComponentImage', 'Component', 'ComponentListener', 'ComponentPaintDiagnostics',
    'ComponentTraverser', 'FocusTraverser', 'ModalCallbackFunction', 'ModalComponentManager',
  ],
  'gui_basics-desktop': [
    'DarkModeSettingListener', 'Desktop', 'Displays', 'FocusChangeListener',
  ],
  'gui_basics-filebrowser': [
    'ContentSharer', 'DirectoryContentsDisplayComponent', 'DirectoryContentsList',
    'FileBrowserComponent', 'FileBrowserListener', 'FileChooser', 'FileChooserDialogBox',
    'FileListComponent', 'FilePreviewComponent', 'FileSearchPathListComponent',
    'FileTreeComponent', 'FilenameComponent', 'FilenameComponentListener',
    'ImagePreviewComponent',
  ],
  'gui_basics-keyboard': [
    'CaretComponent', 'KeyListener', 'KeyPress', 'KeyboardFocusTraverser', 'ModifierKeys',
    'SystemClipboard', 'TextEditorKeyMapper', 'TextInputTarget',
  ],
  'gui_basics-lookandfeel': [
    'ExtraLookAndFeelBaseClasses', 'LookAndFeel', 'LookAndFeel_V1', 'LookAndFeel_V2',
    'LookAndFeel_V3', 'LookAndFeel_V4',
  ],
  'gui_basics-misc': [
    'BubbleComponent', 'DrawableComponent', 'DropShadower', 'FocusOutline',
    'OwningDrawableComponent',
  ],
  'gui_basics-mouse': [
    'ComponentDragger', 'DragAndDropContainer', 'DragAndDropTarget', 'FileDragAndDropTarget',
    'LassoComponent', 'LassoSource', 'MouseCursor', 'MouseEvent', 'MouseInactivityDetector',
    'MouseInputSource', 'MouseListener', 'MouseWheelDetails', 'PenDetails', 'SelectedItemSet',
    'SettableTooltipClient', 'TextDragAndDropTarget', 'TooltipClient',
  ],
  'gui_basics-positioning': [
    'MarkerList', 'RelativeCoordinate', 'RelativeCoordinatePositionerBase',
    'RelativeParallelogram', 'RelativePoint', 'RelativePointPath', 'RelativeRectangle',
  ],
  'audio_processors': [
    'AudioProcessorEditor', 'AudioProcessorEditorHostContext', 'GenericAudioProcessorEditor',
    'HostProvidedContextMenu', 'KnownPluginList', 'PluginDirectoryScanner',
    'PluginListComponent', 'AudioProcessorValueTreeState', 'ButtonParameterAttachment',
    'ComboBoxParameterAttachment', 'ParameterAttachment', 'PluginHostType',
    'SliderParameterAttachment', 'WebComboBoxParameterAttachment',
    'WebSliderParameterAttachment', 'WebToggleButtonParameterAttachment',
    'AudioUnitPluginFormat', 'LADSPAPluginFormat', 'LV2PluginFormat', 'VSTPluginFormat',
    'NSViewComponentWithParent',
  ],
  'gui_basics-buttons': [
    'ArrowButton', 'Button', 'DrawableButton', 'HyperlinkButton', 'ImageButton',
    'ShapeButton', 'TextButton', 'ToggleButton', 'ToolbarButton',
  ],
};

/**
 * Groups NOT yet walked. Listed by name rather than left implicit, because the
 * whole point of this file is that an unexamined area should be visible.
 *
 * dsp is absent from both lists on purpose: it was audited class by class
 * against the reference's own list when its 51 rows were written, before this file
 * existed, and re-entering it here would be transcribing that work rather than
 * checking it.
 */
export const PENDING = [
  // ── Modules whose CLASSES have not been walked ─────────────────────────
  //
  // Every one of these has a decision in the MODULE table at the top of
  // feature-map.mjs -- covered, or out of scope with a reason. That is a
  // coarser instrument than this one: it decides a module in a line, where
  // this decides it a class at a time.
  //
  // The distinction is the whole point and it was nearly lost here. When the
  // ten modules below this list were finished, the tool printed "NOT audited
  // yet (0 areas)" -- which reads as "all of the reference", and meant "all of the
  // areas somebody listed". That is the identical failure this file was
  // written to stop, reproduced one level up, and it survived until the
  // module table was compared against the index by hand.
  //
  // dsp is the one with real work behind it: its 51 rows were audited
  // class by class against the reference's own list before this file existed. It is
  // listed anyway, because "somebody did that once" is not a thing a check can
  // re-run, and that is the difference between an audit and a memory.
  // Decided in the MODULE table with a reason, and their class lists are not
  // indexed here. That is a deliberate stopping point rather than an oversight,
  // and the difference from opengl -- which IS indexed, despite being
  // equally out of scope -- is that these four have nothing whose absence could
  // ever become a question. A plugin will not one day want a physics engine or
  // an analytics client. OpenGL is indexed because "should this rasterise on
  // the GPU" is a question somebody could reasonably reopen.
  'video (module decided: out of scope, class list not indexed)',
  'javascript (module decided: out of scope, class list not indexed)',
  'analytics (module decided: out of scope, class list not indexed)',
  'box2d (module decided: out of scope, class list not indexed)',
];

/**
 * Names that are not capabilities and must not be demanded of the map.
 *
 * Each one is here for a reason that is about the NAME, never about the work:
 * an abstract base with no independent behaviour, a listener interface, or a
 * namespace that the docs index lists as though it were a class. Anything
 * absent for a design reason belongs in the feature map as an absent row with
 * that reason, not here -- this list is only for things a row would be a
 * category error.
 */
export const NOT_CAPABILITIES = {
  SliderListener: 'a listener interface, not a capability: our sliders carry std::function callbacks',
  ListBoxModel: 'the model half of ListBox, which our ListBox expresses as callbacks',
  TableListBoxModel: 'likewise, the model half of TableListBox',
  ToolbarItemFactory: 'the factory half of Toolbar; stands or falls with that row',
  MenuBarModel: 'the model half of MenuBarComponent; stands or falls with that row',
  AnimatedPositionBehaviours: 'a namespace of policy structs for AnimatedPosition, not a class',
  WindowUtils: 'a namespace of free functions, listed by the docs index as though it were a class',
  MessageBoxOptions: 'the argument object of NativeMessageBox; stands or falls with that row',
  ScopedMessageBox: 'the RAII handle of NativeMessageBox; stands or falls with that row',
  PropertyComponent: 'the abstract base of the PropertyComponent family; the family has its own row',
  Button: 'the abstract base every button derives from; the concrete buttons have rows',
  ComponentPeer: 'the reference’s internal name for the platform window; ours are the window_*.h peers',
  ImagePixelData: 'the reference-counted innards of Image, not something a caller names',
  ImageFileFormat: 'the abstract base of the PNG/JPEG/GIF readers; each format has its own row',
  PixelAlpha: 'a single-channel pixel layout, an implementation detail of the reference’s blitters',
  PixelRGB: 'likewise, the opaque 24-bit layout',
  ParameterAttachment: 'the abstract base the concrete attachments derive from; each has its own row',
  AudioProcessorEditorHostContext: 'the accessor that hands over a HostProvidedContextMenu; stands or falls with that row',
  ComponentListener: 'a listener interface; our components carry std::function callbacks',
  FocusChangeListener: 'likewise, for focus',
  FilenameComponentListener: 'likewise, for FilenameComponent',
  FileBrowserListener: 'likewise, for FileBrowserComponent',
  DarkModeSettingListener: 'likewise, for the OS dark-mode setting',
  KeyListener: 'likewise, for keys -- Component::keyPressed is the whole interface here',
  TooltipClient: 'the interface a component implements to HAVE a tooltip; ours is a string on Widget',
  LassoSource: 'the source half of LassoComponent; stands or falls with that row',
  ComponentTraverser: 'the abstract base of the focus traversers; those have their own row',
  DirectoryContentsDisplayComponent: 'the abstract base the file list and tree share; both have rows',
  ExtraLookAndFeelBaseClasses: 'a namespace of per-widget LookAndFeel method groups, not a class',
  ModalCallbackFunction: 'a helper for building modal callbacks; stands or falls with ModalComponentManager',
  ApplicationCommandManagerListener: 'a listener interface; CommandManager here carries callbacks',
  CodeTokeniser: 'the abstract base the syntax tokenisers derive from; the family has one row',
  CppTokeniserFunctions: 'helper functions for CPlusPlusCodeTokeniser; stands or falls with it',
  XEmbedComponentOptions: 'the argument object of XEmbedComponent; stands or falls with that row',
  WebViewLifetimeListener: 'a listener interface for WebBrowserComponent; ours is a callback',
  ActionListener: 'a listener interface; ChangeListener covers the same job here',
  MessageListener: 'a listener interface for MessageManager; stands or falls with that row',
  UndoableAction: 'the abstract action UndoManager stores; ours stores whole states, see that row',
  ScopedLibraryInitialiser: 'an RAII guard for the reference library init; nothing here needs initialising',
  ChildProcessManager: 'the coordinator half of ChildProcessWorker; one row covers the family',
  ChildProcessWorker: 'likewise, the worker half',
  InterprocessConnectionServer: 'the listening half of InterprocessConnection; one row covers both',
  AudioSource: 'the abstract base of the reference\u2019s pull-model sources; the family has one row',
  PositionableAudioSource: 'likewise, the seekable variant of that base',
  SynthesiserSound: 'the sound half of Synthesiser; VoiceManager\u2019s row covers the pair',
  SynthesiserVoice: 'likewise, the voice half',
  MPESynthesiserVoice: 'the MPE flavour of SynthesiserVoice; same row',
  MPEChannelRemapper: 'the output half of MPEChannelAssigner; one row covers both',
  MPEZone: 'one entry in MPEZoneLayout; that row covers it',
  MPEMessages: 'a helper that builds MPE configuration messages; stands or falls with MpeZone',
  ScopedBlendContextOptions: 'the argument object of ScopedBlendContext; stands or falls with it',
  AudioThumbnailBase: 'the abstract base AudioThumbnail derives from; that row covers it',
  KeyboardComponentBase: 'the abstract base the two keyboard components share; both have rows',
  ImageEffectFilter: 'the abstract base the image effects derive from; each effect has a row',
  ReferenceCountedObjectPtr: 'the pointer half of ReferenceCountedObject; one row covers both',
  GenericScopedLock: 'the RAII lock template; stands or falls with CriticalSection',
  TimeSliceClient: 'the callback half of TimeSliceThread; one row covers both',
  ThreadPoolJob: 'the unit of work ThreadPool runs; that row covers it',
  JSONUtils: 'helpers around JSON; stands or falls with that row',
  VariantConverter: 'the trait JSON uses to convert user types; same row',
  XmlAttribute: 'one attribute of an XmlElement; that row covers it',
  UnitTestRunner: 'the runner half of UnitTest; one row covers both',
  AndroidDocumentInfo: 'metadata for AndroidDocument; that row covers it',
  AndroidDocumentIterator: 'likewise, its iterator',
  DirectoryEntry: 'one entry from RangedDirectoryIterator; that row covers it',
  FileInputSource: 'a File wrapped as an InputSource; the stream row covers the family',
  ReferenceCountedArray: 'Array of ReferenceCountedObjectPtr; both halves have rows',
  DrawableShape: 'the abstract base the drawable shapes share; the family has one row',
  FontFeatureTag: 'the four-character tag naming an OpenType feature; that row covers it',
  GlyphArrangementOptions: 'options for GlyphArrangement; stands or falls with that row',
  GlyphLayer: 'the abstract base of the layered glyph renderers; the family has one row',
  AudioFormat: 'the abstract base each format reader derives from; the family has one row',
  AudioFormatWriterOptions: 'the argument object of AudioFormatWriter; that row covers it',
  AudioFormatReaderSource: 'a reader wrapped as a pull-model AudioSource; that family has a row',
  AudioIODeviceCallback: 'the callback interface AudioIODevice calls; that row covers it',
  MidiInputCallback: 'likewise, for MidiInput',
  MidiDeviceListConnection: 'the RAII handle for device-change notification; MidiDeviceInfo covers it',
  ProcessorBase: 'the abstract base a reference::dsp processor derives from; see the ProcessorChain row',
  ProcessContextNonReplacing: 'the out-of-place form of ProcessContextReplacing; one row covers both',
  ConvolutionMessageQueue: 'the background queue Convolution loads impulse responses on; that row covers it',
  // One entry standing for a family, which is the only place this file does
  // that. midi_ci declares roughly sixty Message::* structs -- Discovery,
  // ProfileInquiry, PropertyGetData and so on -- one per message the MIDI-CI
  // wire protocol defines. They are the protocol, not sixty separate
  // capabilities, and the protocol has its own row.
  'Message::*': 'the ~60 wire-message structs of one protocol; the MIDI-CI row covers the protocol',
  OSCException: 'the exception type the OSC parser throws; the OSC rows cover it',
  OSCTypes: 'the tag constants OSC arguments carry; OSCArgument covers them',
};

namespace Showcase.Contracts;
public record Envelope<T>(int V, string Source, string BootId, long Seq, long CapturedMs, T Data);
public record Capability(bool Supported, string Origin, string DeviceStatus, string? Reason = null);
public record Chip(string Model, int RevisionMajor, int RevisionMinor, int Cores, bool Psram);
public record Firmware(string Name, string Version, string IdfVersion, string BoardLabel, string JsonBackend, bool McpEnabled);
public record Limits(int FrameBytes, int BodyBytes, int TaskPageMax, int MaxWsClients);
public record DeviceInfo(int V, string Source, string BootId, Chip Chip, Firmware Firmware, Dictionary<string, Capability> Capabilities, Limits Limits);
public record MemoryPool(string Id, string Label, int TotalBytes, int FreeBytes, int MinFreeBytes, int LargestBlockBytes);
public record Counters(ulong HttpRequests, ulong WsRxFrames, ulong WsTxFrames, ulong WsRxBytes, ulong WsTxBytes, ulong Rejected);
public record Summary(long UptimeMs, int SampleIntervalMs, double CpuBusyPct, double[] CoreBusyPct, MemoryPool[] Memory, int? RssiDbm, int TaskCount, int WsClients, string NetworkStatus, Counters Counters, ExperimentState Experiment, LedState Led, StripState Strip);
public record DeviceTask(string Id, string Name, string State, int Priority, int? CoreAffinity, int StackMinFreeBytes, ulong RuntimeTicks, double? CpuPct);
public record TaskPage(string SnapshotId, int Offset, int? NextOffset, int Total, DeviceTask[] Items);
public record SimEvent(long Id, long CapturedMs, string Level, string Category, string Message);
public record SimState(string Profile, string Scenario, bool Paused, double Speed, int Seed, long TimeMs, string BootId, bool NetworkOffline, long Sequence);
public record SimCommand(string Action, string? Value = null, double? Number = null);
public record ConnectionInfo(string Id, long ConnectedMs, string State, ulong RxFrames, ulong TxFrames);
public record ConnectionPage(int Offset, int? NextOffset, int Total, ConnectionInfo[] Items);


public record ExperimentState(bool Running,string Kind,int DurationMs,int OnMs,int PeriodMs,int Core,int Priority,long ElapsedMs,double ProgressPct,double Pi,ulong Iterations,bool Active,int SampleMs);
public record LedState(bool Supported,bool Enabled,int PeriodMs,int DutyPct,int Gpio,bool ActiveLow,bool Level);

public record StripState(bool Supported,bool Enabled,int Gpio,int Count,string Mode,int Color,int BrightnessPct,int PeriodMs,int[] Pixels,string Driver,string RequestedDriver,string DriverError);


using System.Globalization;
using Showcase.Contracts;
namespace Showcase.Model;

// Pure bounded, deterministic device model. No sockets, browser or ESP-IDF dependencies.
public sealed class DeviceModel {
    private readonly object gate = new();
    private string profile = "c3", scenario = "normal";
    private readonly string sessionId = Guid.NewGuid().ToString("N")[..8];
    private int seed = 32, generation = 1;
    private long timeMs, sequence, eventId;
    private double speed = 1;
    private bool paused;
    private double phase;
    private int freeInternal = 245760, minInternal = 245760, freePsram = 6 * 1024 * 1024, minPsram = 6 * 1024 * 1024;
    private double cpu;
    private bool experimentRunning; private string experimentKind="pi"; private int durationMs=5000,onMs=200,periodMs=1000,experimentCore,experimentPriority=2; private long experimentStart,experimentElapsed,lastPiUpdate; private double pi; private ulong iterations; private bool ledEnabled=true; private int ledPeriod=1000,ledDuty=50;
    private bool stripEnabled=true; private string stripDriver="rmt",stripMode="solid"; private int stripColor=0xffa030,stripBrightness=20,stripPeriod=1000;
    private long offlineUntil;
    private readonly Queue<SimEvent> events = new();
    private readonly long[] runtime = new long[12];
    private readonly int[] stackMin = Enumerable.Repeat(4096, 12).ToArray();
    private readonly string[] names = ["httpd", "wifi", "main", "esp_timer", "sys_evt", "Diagnostics", "Tmr Svc", "IDLE0", "tcpip_task", "bench_worker", "event_loop", "IDLE1"];
    public DeviceModel() { Recalculate(); AddEvent("info", "system", "Модель ESP32-C3 готова. Все системные показания синтетические."); }
    private string BootId => $"sim-{sessionId}-{generation}";
    private int TaskCount => profile == "s3" ? 12 : 10;
    private bool Offline => Environment.TickCount64 < offlineUntil;
    private string SnapshotId => $"{BootId}:{sequence}";
    public bool NetworkOffline { get { lock(gate) return Offline; } }
    public void Advance(double wallMilliseconds, bool force = false) {
        lock(gate) {
            if (paused && !force) return;
            long delta = force ? 1000 : (long)(wallMilliseconds * speed);
            timeMs += delta; sequence++; phase = timeMs / 1000.0;
            if(experimentRunning){experimentElapsed=Math.Min(durationMs,timeMs-experimentStart);if(timeMs-lastPiUpdate>=250){lastPiUpdate=timeMs;if(experimentKind=="pi")for(int i=0;i<4096;i++){pi+=(iterations%2==0?4.0:-4.0)/(2.0*iterations+1);iterations++;}}if(experimentElapsed>=durationMs){experimentRunning=false;AddEvent("info","experiment","Эксперимент завершён");}}
            Recalculate();
            var loads = Loads();
            for(int i=0; i<TaskCount; i++) {
                runtime[i] += (long)(delta * 1000 * loads[i] / 100);
                var used = 1800 + (i * 137 + seed * 11) % 1800 + (int)(Math.Abs(Math.Sin(phase/7+i))*200);
                stackMin[i] = Math.Min(stackMin[i], used);
            }
            if(scenario == "stack") stackMin[0] = Math.Min(stackMin[0], Math.Max(96, 1800 - (int)(phase*18)));
            if(sequence % 15 == 0) AddEvent("info", "diagnostic", $"Снимок #{sequence}: CPU {cpu:F1}%, internal free {freeInternal/1024} КиБ");
        }
    }
    private void Recalculate() {
        double wave = Math.Sin(phase/6 + seed*.13);
        cpu = Math.Round(Math.Clamp((scenario=="cpu" ? 82 : scenario=="traffic" ? 47 : 14) + 5*wave + 2*Math.Sin(phase/2), 2, 98), 1);
        if(ExperimentActive())cpu=Math.Min(98,cpu+70.0/(profile=="s3"?2:1));
        int baseFree = profile=="s3" ? 280000 : 245760;
        freeInternal = Math.Max(18000, baseFree + (int)(1800*wave) - (scenario=="leak" ? (int)(phase*1200) : 0) - (scenario=="traffic" ? 18000 : 0));
        minInternal = Math.Min(minInternal, freeInternal);
        freePsram = 6*1024*1024 + (int)(10000*wave) - (scenario=="leak" ? (int)(phase*3000) : 0);
        freePsram = Math.Max(1024*1024, freePsram); minPsram = Math.Min(minPsram,freePsram);
    }
    private double[] Loads() {
        var result = new double[TaskCount]; double sum=0;
        for(int i=0;i<TaskCount;i++) { if(i==7 || i==11)continue; if(i==9&&!ExperimentActive()){result[i]=0;continue;} result[i]=(i==9?14:i==0 ? (scenario=="traffic"?9:4) : i==1?3:1)+.3*Math.Abs(Math.Sin(phase/4+i));sum+=result[i]; }
        for(int i=0;i<TaskCount;i++) if(i!=7&&i!=11)result[i]=result[i]/sum*cpu;
        result[7]=(100-cpu)/(profile=="s3"?2:1);if(profile=="s3")result[11]=(100-cpu)/2;return result;
    }
    private void AddEvent(string level,string category,string message) {events.Enqueue(new(++eventId,timeMs,level,category,message));while(events.Count>200)events.Dequeue();}
    public SimEvent[] Events(long after=0){lock(gate)return events.Where(e=>e.Id>after).ToArray();}
    public SimState State(){lock(gate)return new(profile,scenario,paused,speed,seed,timeMs,BootId,Offline,sequence);}
    public string? Apply(SimCommand command) {
        lock(gate) {
            switch(command.Action) {
                case "pause":paused=true;break;
                case "run":paused=false;break;
                case "step": if(!paused)return "Pause the model before stepping"; Advance(1000,true);break;
                case "speed":if(command.Number is not (0.25 or 1 or 2 or 5))return "Invalid speed";speed=command.Number.Value;break;
                case "scenario":if(command.Value is not ("normal" or "cpu" or "leak" or "stack" or "weak_wifi" or "traffic"))return "Invalid scenario";scenario=command.Value;break;
                case "profile":if(command.Value is not ("c3" or "s3"))return "Invalid profile";profile=command.Value;Reset();break;
                case "seed":if(command.Number is null || command.Number<0 || command.Number>99999 || command.Number!=Math.Floor(command.Number.Value))return "Invalid seed";seed=(int)command.Number;Reset();break;
                case "reset":Reset();break;
                case "network":offlineUntil=Environment.TickCount64+8000;break;
                default:return "Unknown action";
            }
            Recalculate();AddEvent(command.Action=="network"?"warn":"info","simulator",$"{command.Action} {command.Value ?? command.Number?.ToString(CultureInfo.InvariantCulture) ?? ""}".Trim());return null;
        }
    }
    private void Reset(){stripEnabled=true;stripDriver="rmt";stripMode="solid";stripColor=0xffa030;stripBrightness=20;stripPeriod=1000;experimentRunning=false;experimentElapsed=0;pi=0;iterations=0;generation++;timeMs=0;sequence=0;phase=0;minInternal=profile=="s3"?280000:245760;minPsram=6*1024*1024;offlineUntil=0;Array.Clear(runtime);Array.Fill(stackMin,4096);Recalculate();AddEvent("info","system","Виртуальная перезагрузка: счётчики и история CPU сброшены");}
    public DeviceInfo Info(){lock(gate)return new(2,"simulator",BootId,new(profile=="s3"?"ESP32-S3":"ESP32-C3",0,4,profile=="s3"?2:1,profile=="s3"),new("nawserver_showcase","0.1.0-simulator","v5.4.0","Synthetic device profile","system_text_json",true),new(){["tasks"]=new(true,"simulated","planned"),["experiments"]=new(true,"simulated","planned"),["led"]=new(true,"simulated","planned"),["strip"]=new(true,"simulated","planned"),["memory"]=new(true,"simulated","planned"),["connections"]=new(true,"local","planned"),["simulator_control"]=new(true,"local","not_applicable"),["sse"]=new(true,"local","implemented"),["authenticated_ws"]=new(false,"none","planned","pre_handshake_auth_unavailable")},new(4096,4096,8,4));}
    public Envelope<Summary> Summary(Counters counters,int clients){lock(gate){var pools=new List<MemoryPool>{new("internal","Внутренняя RAM",327680,freeInternal,minInternal,(int)(freeInternal*(scenario=="leak"?.37:.79)))};if(profile=="s3")pools.Add(new("psram","PSRAM",8*1024*1024,freePsram,minPsram,(int)(freePsram*.85)));return new(2,"simulator",BootId,sequence,timeMs,new(timeMs,FastTelemetry?100:1000,cpu,profile=="s3"?[Math.Round(Math.Clamp(cpu+4,0,100),1),Math.Round(Math.Clamp(cpu-4,0,100),1)]:[cpu],pools.ToArray(),Offline?null:scenario=="weak_wifi"?-86:-53+(int)(3*Math.Sin(phase/8)),TaskCount,clients,Offline?"offline":"connected",counters,Experiment(),Led(),Strip()));}}
    public Envelope<TaskPage> Tasks(int offset,int limit,string? expectedSnapshot=null){lock(gate){if(expectedSnapshot is not null&&expectedSnapshot!=SnapshotId)throw new InvalidOperationException("snapshot_changed");var loads=Loads();var items=Enumerable.Range(0,TaskCount).Skip(offset).Take(limit).Select(i=>new DeviceTask($"task-{i}",names[i],i==9?(ExperimentActive()?"running":"blocked"):i==7||i==11?"ready":i==0&&scenario=="cpu"?"running":"blocked",i==9?experimentPriority:i==7||i==11?0:i==1?23:5,i==9?experimentCore:profile=="s3"?i%2:0,stackMin[i],(ulong)runtime[i],sequence==0?null:Math.Round(loads[i],2))).ToArray();return new(2,"simulator",BootId,sequence,timeMs,new(SnapshotId,offset,offset+items.Length<TaskCount?offset+items.Length:null,TaskCount,items));}}
    private bool ExperimentActive()=>experimentRunning&&(experimentKind=="pi"||(timeMs-experimentStart)%periodMs<onMs);
    public ExperimentState Experiment(){lock(gate)return new(experimentRunning,experimentKind,durationMs,onMs,periodMs,experimentCore,experimentPriority,experimentElapsed,Math.Round(100.0*experimentElapsed/durationMs,1),pi,iterations,ExperimentActive(),100);}
    public LedState Led(){lock(gate)return new(true,ledEnabled,ledPeriod,ledDuty,8,true,ledEnabled&&timeMs%ledPeriod<ledPeriod*ledDuty/100.0);}
    public string? SetLed(bool enabled,int period,int duty){lock(gate){if(period<20||period>5000||duty<0||duty>100)return "LED bounds";ledEnabled=enabled;ledPeriod=period;ledDuty=duty;AddEvent("info","led",$"LED: {period} ms / {duty}% (модель GPIO8 active-low)");return null;}}
    public string? SetExperiment(string action,string kind,int duration,int activeMs,int period,int core,int priority){lock(gate){if(action=="stop"){experimentRunning=false;Recalculate();AddEvent("info","experiment","Эксперимент остановлен");return null;}if(action!="start"||kind is not ("pi" or "pulse")||duration<1000||duration>30000||period<20||period>5000||activeMs<1||activeMs>period||core<0||core>=(profile=="s3"?2:1)||priority<1||priority>5)return "Experiment bounds";if(experimentRunning)return "Already running";experimentKind=kind;durationMs=duration;onMs=activeMs;periodMs=period;experimentCore=core;experimentPriority=priority;experimentStart=timeMs;experimentElapsed=0;pi=0;iterations=0;lastPiUpdate=timeMs;experimentRunning=true;AddEvent("info","experiment",$"Запуск {kind}, {duration} ms / core {core} / priority {priority}. Синтетическая нагрузка.");Recalculate();return null;}}
    public bool FastTelemetry {get{lock(gate)return experimentRunning||(stripEnabled&&stripMode!="solid");}}
    public StripState Strip(){lock(gate){var pixels=new int[10];for(int i=0;i<pixels.Length;i++){if(!stripEnabled)continue;int color=stripColor;if(stripMode=="chase"&&i!=(int)(timeMs/stripPeriod%10))continue;if(stripMode=="rainbow"){double hue=(i/10.0+timeMs/(double)stripPeriod)%1;double h=hue*6,x=1-Math.Abs(h%2-1);var rgb=(int)h switch{0=>(1.0,x,0.0),1=>(x,1.0,0.0),2=>(0.0,1.0,x),3=>(0.0,x,1.0),4=>(x,0.0,1.0),_=>(1.0,0.0,x)};color=((int)(rgb.Item1*255)<<16)|((int)(rgb.Item2*255)<<8)|(int)(rgb.Item3*255);}pixels[i]=(((color>>16)&255)*stripBrightness/100<<16)|(((color>>8)&255)*stripBrightness/100<<8)|((color&255)*stripBrightness/100);}return new(true,stripEnabled,2,10,stripMode,stripColor,stripBrightness,stripPeriod,pixels,stripDriver,stripDriver,"");}}
    public string? SetStrip(bool enabled,string mode,int color,int brightness,int period,string? driver=null){lock(gate){if(mode is not ("solid" or "chase" or "rainbow")||(driver is not null && driver is not ("rmt" or "spi"))||color<0||color>0xffffff||brightness<0||brightness>100||period<20||period>5000)return "Strip bounds";stripEnabled=enabled;if(driver is not null)stripDriver=driver;stripMode=mode;stripColor=color;stripBrightness=brightness;stripPeriod=period;AddEvent("info","strip",$"WS2812: 10 pixels / GPIO2 / {mode} / {brightness}% (модель)");return null;}}}






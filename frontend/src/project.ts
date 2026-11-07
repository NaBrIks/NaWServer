import type {Component} from 'vue';
export interface ProjectPage {
 id:string;
 icon:string;
 label:{ru:string;en:string};
 component:Component;
}
export interface FrontendProject {
 label:string;
 storageKey:string;
 admissionPreflight:boolean;
 pages:ProjectPage[];
 platform?:{
  deviceLabel:string;
  chipFallback:string;
  sdkLabel:string;
  internalMemoryLabel?:{ru:string;en:string};
  workloadPriorityMax?:number;
  ledController?:string;
  detailLabels?:Record<string,[string,string]>;
 };
}
export const defaultPlatform={deviceLabel:'Device',chipFallback:'—',sdkLabel:'SDK',workloadPriorityMax:1};
export const espPlatform={deviceLabel:'ESP32',chipFallback:'ESP32',sdkLabel:'ESP-IDF',workloadPriorityMax:5};

export class Configuration {
  constructor(
    public connectionMode: number,
    public wifiSsid: string,
    public wifiPassword: string,
    public ledBrightness: number,
    public ledColorR: number,
    public ledColorG: number,
    public ledColorB: number,
    public timezoneLocation: string,
    public showDate: boolean,
    public showHimidity: boolean,
    public showTemperature: boolean
  ) {}
}

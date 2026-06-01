import { element } from "../components/dom";

export function sparkline(values: number[], label: string): HTMLElement {
  const wrapper = element("figure", "sparkline");
  const svg = document.createElementNS("http://www.w3.org/2000/svg", "svg");
  svg.setAttribute("viewBox", "0 0 240 64");
  svg.setAttribute("role", "img");
  svg.setAttribute("aria-label", label);

  const usable = values.filter((value) => Number.isFinite(value));
  const min = Math.min(...usable, 0);
  const max = Math.max(...usable, 1);
  const span = Math.max(1, max - min);
  const step = usable.length > 1 ? 240 / (usable.length - 1) : 240;
  const points = usable
    .map((value, index) => {
      const x = index * step;
      const y = 58 - ((value - min) / span) * 52;
      return `${x.toFixed(2)},${y.toFixed(2)}`;
    })
    .join(" ");

  const line = document.createElementNS("http://www.w3.org/2000/svg", "polyline");
  line.setAttribute("points", points);
  line.setAttribute("fill", "none");
  line.setAttribute("stroke", "currentColor");
  line.setAttribute("stroke-width", "2");
  line.setAttribute("stroke-linecap", "round");
  line.setAttribute("stroke-linejoin", "round");
  svg.appendChild(line);

  wrapper.appendChild(svg);
  return wrapper;
}

import { element } from "./dom";

export function objectTable(
  rows: Array<Record<string, unknown>>,
  emptyLabel: string
): HTMLElement {
  const wrapper = element("div", "table-wrap");
  if (rows.length === 0) {
    wrapper.appendChild(element("p", "empty", emptyLabel));
    return wrapper;
  }

  const columns = Object.keys(rows[0] ?? {}).slice(0, 8);
  const table = document.createElement("table");
  table.className = "data-table";

  const thead = document.createElement("thead");
  const headRow = document.createElement("tr");
  for (const column of columns) {
    const th = document.createElement("th");
    th.textContent = column;
    headRow.appendChild(th);
  }
  thead.appendChild(headRow);
  table.appendChild(thead);

  const tbody = document.createElement("tbody");
  for (const row of rows) {
    const tr = document.createElement("tr");
    for (const column of columns) {
      const td = document.createElement("td");
      const value = row[column];
      td.textContent =
        typeof value === "object" && value != null
          ? JSON.stringify(value)
          : String(value ?? "");
      tr.appendChild(td);
    }
    tbody.appendChild(tr);
  }
  table.appendChild(tbody);
  wrapper.appendChild(table);
  return wrapper;
}

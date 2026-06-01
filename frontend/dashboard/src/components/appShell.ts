import { clear, element } from "./dom";
import type { PageDefinition } from "../pages/pageTypes";
import type { DashboardUiState } from "../state/dashboardStore";

export class AppShell {
  private activePageId: PageDefinition["id"] = "overview";
  private readonly nav = element("nav", "sidebar-nav");
  private readonly content = element("main", "content");

  constructor(
    private readonly root: HTMLElement,
    private readonly pages: PageDefinition[]
  ) {
    this.mount();
  }

  render(state: DashboardUiState): void {
    this.renderNav();
    const page =
      this.pages.find((candidate) => candidate.id === this.activePageId) ??
      this.pages[0];
    clear(this.content);
    this.content.appendChild(page.render(state));
  }

  private mount(): void {
    clear(this.root);
    const layout = element("div", "dashboard-shell");
    const sidebar = element("aside", "sidebar");
    sidebar.appendChild(element("div", "brand", "Polytope"));
    sidebar.appendChild(element("div", "brand-subtitle", "Paper Dashboard"));
    sidebar.appendChild(this.nav);
    layout.appendChild(sidebar);
    layout.appendChild(this.content);
    this.root.appendChild(layout);
  }

  private renderNav(): void {
    clear(this.nav);
    for (const page of this.pages) {
      const button = document.createElement("button");
      button.type = "button";
      button.className =
        page.id === this.activePageId ? "nav-button active" : "nav-button";
      button.textContent = page.title;
      button.addEventListener("click", () => {
        this.activePageId = page.id;
        this.renderNav();
        window.dispatchEvent(new CustomEvent("dashboard:navigate"));
      });
      this.nav.appendChild(button);
    }
  }
}
